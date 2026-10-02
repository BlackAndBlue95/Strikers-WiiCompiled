using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Loading;

namespace Translator.Core.Disassembly;

internal static class JumpTableDetector
{
    private const int MaxBacktrackInstructions = 192;
    private const int MaxEntryCount = 512;

    public static bool TryRecognize(
        IReadOnlyList<PpcInstruction> ordered,
        IReadOnlyDictionary<uint, int> indexByAddress,
        uint bctrAddress,
        uint functionStart,
        uint functionEnd,
        ProgramImage image,
        out IReadOnlyList<uint> targets)
    {
        targets = Array.Empty<uint>();
        if (!indexByAddress.TryGetValue(bctrAddress, out var bctrIndex))
        {
            return false;
        }

        var mtctrIndex = FindPrevious(ordered, bctrIndex, "mtctr", limit: 6);
        if (mtctrIndex < 0)
        {
            return false;
        }

        if (ordered[mtctrIndex].Operands.Count == 0 || ordered[mtctrIndex].Operands[0] is not PpcRegisterOperand ctrOperand)
        {
            return false;
        }
        var targetRegister = ctrOperand.Name;

        var loadIndex = FindPreviousLoad(ordered, mtctrIndex, targetRegister);
        if (loadIndex < 0)
        {
            return false;
        }

        var load = ordered[loadIndex];
        if (!string.Equals(load.Mnemonic, "lwzx", StringComparison.OrdinalIgnoreCase))
        {
            return false; // Only handle lwzx-based tables for now.
        }

        if (load.Operands.Count < 3 || load.Operands[0] is not PpcRegisterOperand dest ||
            load.Operands[1] is not PpcRegisterOperand baseReg ||
            load.Operands[2] is not PpcRegisterOperand indexReg)
        {
            return false;
        }

        if (!string.Equals(dest.Name, targetRegister, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var entriesAreRelative = false;
        if (!TryResolveLisAddiConstant(ordered, loadIndex, baseReg.Name, out var tableBase) &&
            !TryResolvePcRelativeLoadedConstant(ordered, loadIndex, baseReg.Name, image, out tableBase))
        {
            return false;
        }
        entriesAreRelative = HasRelativeEntryAdd(ordered, loadIndex, mtctrIndex, targetRegister, baseReg.Name);
        if (!OnlyRelativeEntryAddsBetween(ordered, loadIndex, mtctrIndex, targetRegister, baseReg.Name))
        {
            return false;
        }

        if (!TryFindUpperBound(ordered, loadIndex, indexReg.Name, out var upperBound) || upperBound < 0 || upperBound >= MaxEntryCount)
        {
            return false;
        }

        var entryCount = upperBound + 1;
        var dedupList = new List<uint>();
        var seen = new HashSet<uint>();
        for (var i = 0; i < entryCount; i++)
        {
            var entryAddress = unchecked(tableBase + (uint)(i * 4));
            if (!TryReadWord(image, entryAddress, out var entry))
            {
                return false;
            }
            var target = entriesAreRelative ? unchecked(tableBase + entry) : entry;

            if (!indexByAddress.ContainsKey(target))
            {
                // Allow targets that haven't been decoded yet as long as they
                // fall within the current function's decoding window.
                if (target < functionStart || target >= functionEnd || (target & 3) != 0)
                {
                    return false;
                }
            }

            if (seen.Add(target))
            {
                dedupList.Add(target);
            }
        }

        if (dedupList.Count == 0)
        {
            return false; // Not a real switch.
        }

        targets = dedupList;
        return true;
    }

    private static int FindPrevious(IReadOnlyList<PpcInstruction> ordered, int startIndex, string mnemonic, int limit)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= limit; i--)
        {
            // Comparison already ignores case, so lowering the needle only
            // allocated a copy of it.
            if (string.Equals(ordered[i].Mnemonic, mnemonic, StringComparison.OrdinalIgnoreCase))
            {
                return i;
            }
        }
        return -1;
    }

    private static bool HasRelativeEntryAdd(
        IReadOnlyList<PpcInstruction> ordered,
        int loadIndex,
        int mtctrIndex,
        string targetRegister,
        string tableBaseRegister)
    {
        for (var i = loadIndex + 1; i < mtctrIndex; i++)
        {
            var ins = ordered[i];
            if (!string.Equals(ins.Mnemonic, "add", StringComparison.OrdinalIgnoreCase) ||
                ins.Operands.Count < 3 ||
                ins.Operands[0] is not PpcRegisterOperand dest ||
                ins.Operands[1] is not PpcRegisterOperand left ||
                ins.Operands[2] is not PpcRegisterOperand right ||
                !RegistersEqual(dest.Name, targetRegister))
            {
                continue;
            }

            if ((RegistersEqual(left.Name, targetRegister) && RegistersEqual(right.Name, tableBaseRegister)) ||
                (RegistersEqual(right.Name, targetRegister) && RegistersEqual(left.Name, tableBaseRegister)))
            {
                return true;
            }
        }

        return false;
    }

    // Between the entry load and mtctr, the target register may only be rebased onto the table
    // (add rT, rT, rBase): any other write means the loaded entry isn't what reaches the CTR.
    private static bool OnlyRelativeEntryAddsBetween(
        IReadOnlyList<PpcInstruction> ordered,
        int loadIndex,
        int mtctrIndex,
        string targetRegister,
        string tableBaseRegister)
    {
        for (var i = loadIndex + 1; i < mtctrIndex; i++)
        {
            var ins = ordered[i];
            if (!WritesRegister(ins, targetRegister))
            {
                continue;
            }

            if (!IsAddKeeping(ins, targetRegister) ||
                ins.Operands[1] is not PpcRegisterOperand left || ins.Operands[2] is not PpcRegisterOperand right ||
                !(RegistersEqual(left.Name, tableBaseRegister) || RegistersEqual(right.Name, tableBaseRegister)))
            {
                return false;
            }
        }

        return true;
    }

    // add rT, rT, rX or add rT, rX, rT.
    private static bool IsAddKeeping(PpcInstruction ins, string register) =>
        ins.Mnemonic == "add" && ins.Operands.Count >= 3 &&
        ins.Operands[0] is PpcRegisterOperand dest && RegistersEqual(dest.Name, register) &&
        ins.Operands[1] is PpcRegisterOperand left && ins.Operands[2] is PpcRegisterOperand right &&
        (RegistersEqual(left.Name, register) || RegistersEqual(right.Name, register));

    private static int FindPreviousLoad(IReadOnlyList<PpcInstruction> ordered, int startIndex, string register)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (!WritesRegister(ins, register) || IsAddKeeping(ins, register))
            {
                // An add that keeps the register as an input is the relative-entry add; it is
                // checked against the table base once that is known.
                continue;
            }

            // The nearest other write decides; decoder mnemonics are lowercase by construction.
            return ins.Mnemonic is "lwzx" or "lwz" && ins.Operands[0] is PpcRegisterOperand dest &&
                   RegistersEqual(dest.Name, register)
                ? i
                : -1;
        }

        return -1;
    }

    private static bool TryResolveLisAddiConstant(IReadOnlyList<PpcInstruction> ordered, int startIndex, string register, out uint value)
    {
        var haveOffset = false;
        int offset = 0;
        var baseRegister = register;
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (!WritesRegister(ins, baseRegister))
            {
                continue;
            }

            var mnemonic = ins.Mnemonic;
            if (!haveOffset && mnemonic == "addi" && ins.Operands.Count >= 3 && ins.Operands[1] is PpcRegisterOperand addBase &&
                ins.Operands[2] is PpcImmediateOperand addImm)
            {
                offset = (short)addImm.Value;
                haveOffset = true;
                // Track the source register so we can resolve lis/addi from a different base.
                baseRegister = addBase.Name;
                continue;
            }

            if (mnemonic == "lis" && ins.Operands.Count >= 2 && ins.Operands[1] is PpcImmediateOperand hiImm)
            {
                var hi = hiImm.Value << 16;
                var low = haveOffset ? offset : 0;
                value = unchecked((uint)(hi + low));
                return true;
            }

            // Anything else wrote the register, so it doesn't hold a lis/addi constant here.
            break;
        }

        value = 0;
        return false;
    }

    private static bool TryResolvePcRelativeLoadedConstant(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        string register,
        ProgramImage image,
        out uint value)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (!string.Equals(ins.Mnemonic, "lwz", StringComparison.OrdinalIgnoreCase) ||
                ins.Operands.Count < 2 ||
                ins.Operands[0] is not PpcRegisterOperand dest ||
                ins.Operands[1] is not PpcDisplacementOperand displacement ||
                !RegistersEqual(dest.Name, register))
            {
                continue;
            }

            if (!TryResolvePicBaseRegister(ordered, i, displacement.BaseRegister, image, out var picBase))
            {
                continue;
            }

            var literalAddress = AddSigned(picBase, displacement.Offset);
            if (TryReadWord(image, literalAddress, out value))
            {
                return true;
            }
        }

        value = 0;
        return false;
    }

    private static bool TryResolvePicBaseRegister(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        string register,
        ProgramImage image,
        out uint value)
    {
        for (var addIndex = startIndex - 1; addIndex >= 0 && startIndex - addIndex <= MaxBacktrackInstructions; addIndex--)
        {
            var add = ordered[addIndex];
            if (!string.Equals(add.Mnemonic, "add", StringComparison.OrdinalIgnoreCase) ||
                add.Operands.Count < 3 ||
                add.Operands[0] is not PpcRegisterOperand dest ||
                add.Operands[1] is not PpcRegisterOperand left ||
                add.Operands[2] is not PpcRegisterOperand right ||
                !RegistersEqual(dest.Name, register))
            {
                continue;
            }

            string offsetRegister;
            if (RegistersEqual(left.Name, register))
            {
                offsetRegister = right.Name;
            }
            else if (RegistersEqual(right.Name, register))
            {
                offsetRegister = left.Name;
            }
            else
            {
                continue;
            }

            if (!TryFindRegisterDisplacementLoadBefore(ordered, addIndex, offsetRegister, register, out var offsetLoad) ||
                !TryFindMflrBefore(ordered, offsetLoad.Index, register, out var mflrIndex) ||
                !TryFindLinkCallBefore(ordered, mflrIndex, out var linkAddress))
            {
                continue;
            }

            var offsetAddress = AddSigned(linkAddress, offsetLoad.Displacement);
            if (!TryReadWord(image, offsetAddress, out var picOffset))
            {
                continue;
            }

            value = unchecked(linkAddress + picOffset);
            return true;
        }

        value = 0;
        return false;
    }

    private static bool TryFindRegisterDisplacementLoadBefore(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        string destinationRegister,
        string baseRegister,
        out (int Index, int Displacement) result)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (string.Equals(ins.Mnemonic, "lwz", StringComparison.OrdinalIgnoreCase) &&
                ins.Operands.Count >= 2 &&
                ins.Operands[0] is PpcRegisterOperand dest &&
                ins.Operands[1] is PpcDisplacementOperand displacement &&
                RegistersEqual(dest.Name, destinationRegister) &&
                RegistersEqual(displacement.BaseRegister, baseRegister))
            {
                result = (i, displacement.Offset);
                return true;
            }
        }

        result = default;
        return false;
    }

    private static bool TryFindMflrBefore(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        string destinationRegister,
        out int mflrIndex)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (string.Equals(ins.Mnemonic, "mflr", StringComparison.OrdinalIgnoreCase) &&
                ins.Operands.Count >= 1 &&
                ins.Operands[0] is PpcRegisterOperand dest &&
                RegistersEqual(dest.Name, destinationRegister))
            {
                mflrIndex = i;
                return true;
            }
        }

        mflrIndex = -1;
        return false;
    }

    private static bool TryFindLinkCallBefore(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        out uint linkAddress)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (ins.IsCall)
            {
                linkAddress = ins.EndAddress;
                return true;
            }
        }

        linkAddress = 0;
        return false;
    }

    private static bool TryFindUpperBound(IReadOnlyList<PpcInstruction> ordered, int startIndex, string register, out int upperBound)
    {
        // Preferred: the compare on the register used for lwzx indexing, followed back through the
        // shift that scales it to a word offset (or a plain move), and never past another write.
        var tracked = register;
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (TryCompareImmediate(ins, out var compared, out var bound) && RegistersEqual(compared, tracked))
            {
                upperBound = bound;
                return true;
            }

            if (!WritesRegister(ins, tracked))
            {
                continue;
            }

            if (!TryScaledOrMovedSource(ins, out tracked))
            {
                break;
            }
        }

        // Fallback: some loops compare a logical counter (e.g. r18) distinct from the byte-offset
        // register (e.g. r30) that feeds lwzx, so the bound check may not target the index register.
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            if (!TryCompareImmediate(ordered[i], out _, out var bound) || bound < 0 || bound >= MaxEntryCount)
            {
                continue;
            }

            upperBound = bound;
            return true;
        }

        upperBound = 0;
        return false;
    }

    // cmpwi / cmplwi [crN,] rA, imm: the decoder puts a CR field first unless it is cr0.
    private static bool TryCompareImmediate(PpcInstruction ins, out string register, out int immediate)
    {
        register = string.Empty;
        immediate = 0;
        if (ins.Mnemonic is not ("cmplwi" or "cmpwi"))
        {
            return false;
        }

        var first = ins.Operands.Count > 0 && ins.Operands[0] is PpcConditionRegisterOperand ? 1 : 0;
        if (ins.Operands.Count < first + 2 ||
            ins.Operands[first] is not PpcRegisterOperand compared ||
            ins.Operands[first + 1] is not PpcImmediateOperand value)
        {
            return false;
        }

        register = compared.Name;
        immediate = value.Value;
        return true;
    }

    // rlwinm rA, rS, 2, 0, 29 (slwi rA, rS, 2) or mr rA, rS: rA holds rS, as a word offset or as is.
    private static bool TryScaledOrMovedSource(PpcInstruction ins, out string source)
    {
        source = string.Empty;
        if (ins.Mnemonic == "slwi" && ins.Operands.Count == 3 &&
            ins.Operands[1] is PpcRegisterOperand slwiSource &&
            ins.Operands[2] is PpcImmediateOperand { Value: 2 })
        {
            source = slwiSource.Name;
            return true;
        }

        if (ins.Mnemonic == "rlwinm" && ins.Operands.Count == 5 &&
            ins.Operands[1] is PpcRegisterOperand shifted &&
            ins.Operands[2] is PpcImmediateOperand { Value: 2 } &&
            ins.Operands[3] is PpcImmediateOperand { Value: 0 } &&
            ins.Operands[4] is PpcImmediateOperand { Value: 29 })
        {
            source = shifted.Name;
            return true;
        }

        if (ins.Mnemonic == "mr" && ins.Operands.Count >= 2 && ins.Operands[1] is PpcRegisterOperand moved)
        {
            source = moved.Name;
            return true;
        }

        return false;
    }

    // Whether `ins` writes `register`. The backward searches stop at the nearest write: one that
    // stepped past it would read a value the register no longer holds.
    private static bool WritesRegister(PpcInstruction ins, string register)
    {
        if (ins.IsCall)
        {
            return IsVolatileGpr(register);
        }

        var mnemonic = ins.Mnemonic;
        if (IsUpdateForm(mnemonic) && ins.Operands.Count >= 2 &&
            ((ins.Operands[1] is PpcDisplacementOperand displacement && RegistersEqual(displacement.BaseRegister, register)) ||
             (ins.Operands[1] is PpcRegisterOperand updatedBase && RegistersEqual(updatedBase.Name, register))))
        {
            return true;
        }

        if (ins.Operands.Count == 0 || ins.Operands[0] is not PpcRegisterOperand destination)
        {
            return false;
        }

        if (mnemonic == "lmw")
        {
            return register.Length >= 2 && register[0] == 'r' &&
                   int.TryParse(register.AsSpan(1), out var number) && number >= destination.Number;
        }

        return RegistersEqual(destination.Name, register) && !ReadsFirstOperand(mnemonic);
    }

    // Instructions whose first register operand is a source: stores, compares, mt*, traps, cache ops.
    private static bool ReadsFirstOperand(string mnemonic) =>
        mnemonic.StartsWith("st", StringComparison.Ordinal) ||
        mnemonic.StartsWith("psq_st", StringComparison.Ordinal) ||
        mnemonic.StartsWith("cmp", StringComparison.Ordinal) ||
        mnemonic.StartsWith("mt", StringComparison.Ordinal) ||
        mnemonic.StartsWith("tw", StringComparison.Ordinal) ||
        mnemonic.StartsWith("dcb", StringComparison.Ordinal) ||
        mnemonic.StartsWith("icb", StringComparison.Ordinal);

    // Loads and stores with update (lwzu, stwux, psq_lu, ...) also write their base register.
    private static bool IsUpdateForm(string mnemonic) =>
        (mnemonic.StartsWith('l') || mnemonic.StartsWith("st", StringComparison.Ordinal) ||
         mnemonic.StartsWith("psq_", StringComparison.Ordinal)) &&
        (mnemonic.EndsWith('u') || mnemonic.EndsWith("ux", StringComparison.Ordinal));

    // r0 and r3-r12, which a call may clobber.
    private static bool IsVolatileGpr(string register) =>
        register.Length >= 2 && register[0] == 'r' && int.TryParse(register.AsSpan(1), out var number) &&
        (number == 0 || number is >= 3 and <= 12);

    private static bool TryReadWord(ProgramImage image, uint address, out uint value)
    {
        if (!image.Contains(address, sizeof(uint)))
        {
            value = 0;
            return false;
        }

        var span = image.Memory.AsSpan(image.GetOffset(address, sizeof(uint)), sizeof(uint));
        value = BinaryPrimitives.ReadUInt32BigEndian(span);
        return true;
    }

    private static bool RegistersEqual(string left, string right) =>
        string.Equals(left, right, StringComparison.OrdinalIgnoreCase);

    private static uint AddSigned(uint value, int offset) =>
        unchecked(value + (uint)offset);

}
