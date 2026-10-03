using System.Buffers.Binary;
using System.Security.Cryptography;

namespace Translator.Core.Parsing.Kamek;

/// <summary>Where one module of a <see cref="KamekModuleSet"/> sits in the folded image.</summary>
public sealed record KamekModuleSetEntry(
    string Name,
    string Sha256,
    uint Offset,
    uint CodeSize,
    uint BssSize,
    int CtorCount);

/// <summary>
/// Several Kamek modules laid out one after another as a single module, so the mod pipeline (which
/// translates one chunk) handles every installed code mod in one pass. The folded chunk is a valid
/// Kamek chunk in its own right:
/// <list type="bullet">
/// <item>module <c>i</c>'s code starts at <c>Offset</c> (0x20-aligned), followed by its BSS as zeros, so
/// the folded chunk has no BSS of its own;</item>
/// <item>every module-relative command address and module-relative target is moved by <c>Offset</c>;
/// absolute ones are kept. Commands stay in module order, so where two modules write the same address
/// the later module wins, as it would loading after the first on a console;</item>
/// <item>a new constructor table at the end lists every module's constructors in module order, each
/// entry an Addr32 relocation, and is the folded chunk's ctor range.</item>
/// </list>
/// </summary>
public sealed record KamekModuleSet(KamekChunk Chunk, IReadOnlyList<KamekModuleSetEntry> Modules)
{
    public const uint ModuleAlignment = 0x20;

    /// <summary>Folds raw Kamek modules (v2 or v3, one chunk each), in the order given.</summary>
    public static KamekModuleSet Fold(IReadOnlyList<(string Name, byte[] Data)> modules)
    {
        if (modules.Count == 0)
        {
            throw new ArgumentException("A module set needs at least one module.", nameof(modules));
        }

        var code = new List<byte>();
        var commands = new List<KamekCommand>();
        var ctorTargets = new List<uint>();
        var entries = new List<KamekModuleSetEntry>(modules.Count);
        foreach (var (name, data) in modules)
        {
            var file = KamekPulFile.Parse(data);
            if (file.IsCombined)
            {
                throw new InvalidDataException(
                    $"{name} is a multi-region Code.pul; a code mod is a single Kamek module (Kamek -dynamic -output-kamek).");
            }

            var chunk = file.Chunks[0];
            var offset = AlignUp((uint)code.Count, ModuleAlignment);
            PadTo(code, offset);
            code.AddRange(chunk.CodeBlob);
            PadTo(code, checked(offset + chunk.CodeSize + chunk.BssSize));

            foreach (var command in chunk.Commands)
            {
                commands.Add(Move(command, offset));
            }

            var ctorCount = 0;
            for (var slot = chunk.CtorStart; slot + 4 <= chunk.CtorEnd; slot += 4)
            {
                if (CtorTarget(chunk, slot) is { } target)
                {
                    ctorTargets.Add(target >= 0x80000000u ? target : checked(target + offset));
                    ctorCount++;
                }
            }

            entries.Add(new KamekModuleSetEntry(
                name,
                Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant(),
                offset,
                chunk.CodeSize,
                chunk.BssSize,
                ctorCount));
        }

        var ctorStart = AlignUp((uint)code.Count, 4);
        PadTo(code, ctorStart);
        for (var index = 0; index < ctorTargets.Count; index++)
        {
            var slot = checked(ctorStart + (uint)index * 4);
            code.AddRange(new byte[4]);
            commands.Add(Command(KamekCommandId.Addr32, absolute: false, slot, [ctorTargets[index]]));
        }

        var codeSize = (uint)code.Count;
        if (codeSize >= 0x00FFFFFEu)
        {
            throw new InvalidDataException($"The code mods add up to 0x{codeSize:X} bytes, more than a Kamek module can address.");
        }

        var chunkSize = checked((uint)(KamekChunk.HeaderSize + code.Count + commands.Sum(EncodedSize)));
        var folded = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: codeSize,
            ctorStart: ctorStart,
            ctorEnd: codeSize,
            chunkSize: chunkSize,
            codeBlob: code.ToArray(),
            commands: commands);
        return new KamekModuleSet(folded, entries);
    }

    /// <summary>The folded chunk as a raw Kamek v3 file, which <see cref="KamekPulFile.Parse"/> reads back.</summary>
    public byte[] Serialize()
    {
        var data = new byte[Chunk.ChunkSize];
        var span = data.AsSpan();
        BinaryPrimitives.WriteUInt32BigEndian(span[0x00..], KamekChunk.Magic0);
        BinaryPrimitives.WriteUInt32BigEndian(span[0x04..], KamekChunk.Magic1);
        BinaryPrimitives.WriteUInt32BigEndian(span[0x08..], Chunk.BssSize);
        BinaryPrimitives.WriteUInt32BigEndian(span[0x0C..], Chunk.CodeSize);
        BinaryPrimitives.WriteUInt32BigEndian(span[0x10..], Chunk.CtorStart);
        BinaryPrimitives.WriteUInt32BigEndian(span[0x14..], Chunk.CtorEnd);
        BinaryPrimitives.WriteUInt32BigEndian(span[0x18..], Chunk.ChunkSize);
        Chunk.CodeBlob.CopyTo(span[KamekChunk.HeaderSize..]);

        var position = KamekChunk.HeaderSize + Chunk.CodeBlob.Length;
        foreach (var command in Chunk.Commands)
        {
            BinaryPrimitives.WriteUInt32BigEndian(span[position..], command.CommandWord);
            position += 4;
            if (command.AddressIsAbsolute)
            {
                BinaryPrimitives.WriteUInt32BigEndian(span[position..], command.Address);
                position += 4;
            }
            foreach (var argument in command.Arguments)
            {
                BinaryPrimitives.WriteUInt32BigEndian(span[position..], argument);
                position += 4;
            }
        }
        return data;
    }

    // The commands whose first argument is an address (module-relative below 0x80000000), as Kamek's
    // own loader resolves them; the other arguments are literal values.
    private static bool FirstArgumentIsAddress(KamekCommandId id) => id is
        KamekCommandId.Addr32 or KamekCommandId.Addr16Lo or KamekCommandId.Addr16Hi or KamekCommandId.Addr16Ha or
        KamekCommandId.Rel24 or KamekCommandId.CondWritePointer or KamekCommandId.Branch or KamekCommandId.BranchLink;

    private static KamekCommand Move(KamekCommand command, uint offset)
    {
        var address = command.AddressIsAbsolute ? command.Address : checked(command.Address + offset);
        var arguments = command.Arguments.ToArray();
        if (arguments.Length > 0 && FirstArgumentIsAddress(command.Id) && arguments[0] < 0x80000000u)
        {
            arguments[0] = checked(arguments[0] + offset);
        }
        return Command(command.Id, command.AddressIsAbsolute, address, arguments);
    }

    // A constructor slot's target: the relocation Kamek wrote for it (the last one, as the loader
    // applies them in order), or the slot's own word when nothing relocates it; null for an empty slot.
    private static uint? CtorTarget(KamekChunk chunk, uint slot)
    {
        var relocation = chunk.Commands.LastOrDefault(command =>
            command.AddressIsRelative && command.Address == slot && command.Id == KamekCommandId.Addr32);
        if (relocation is not null)
        {
            return relocation.Arguments[0];
        }

        var word = BinaryPrimitives.ReadUInt32BigEndian(chunk.CodeBlob.AsSpan(checked((int)slot), 4));
        return word == 0 ? null : word;
    }

    private static KamekCommand Command(KamekCommandId id, bool absolute, uint address, uint[] arguments)
    {
        if (!absolute && address >= 0x00FFFFFEu)
        {
            throw new InvalidDataException($"Module offset 0x{address:X} doesn't fit a Kamek command.");
        }
        var word = ((uint)(byte)id << 24) | (absolute ? 0x00FFFFFEu : address);
        return new KamekCommand(0, word, id, absolute, address, arguments);
    }

    private static int EncodedSize(KamekCommand command) =>
        4 + (command.AddressIsAbsolute ? 4 : 0) + command.Arguments.Count * 4;

    private static void PadTo(List<byte> bytes, uint length)
    {
        while (bytes.Count < length)
        {
            bytes.Add(0);
        }
    }

    private static uint AlignUp(uint value, uint alignment) => checked((value + alignment - 1) & ~(alignment - 1));
}
