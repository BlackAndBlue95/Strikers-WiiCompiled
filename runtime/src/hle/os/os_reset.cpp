#include "memory.h"
#include "hle/audio/ax_dsp.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "system_bridge.h"

#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <string>
#include <chrono>
#include <thread>

namespace {

static std::string ReadGuestStringSafe(uint32_t addr)
{
    if (addr == 0) {
        return "";
    }
    std::string out;
    out.reserve(64);
    try {
        for (size_t i = 0; i < 512; ++i) {
            uint8_t b = Memory::Read8(addr + static_cast<uint32_t>(i));
            if (b == 0) break;
            out.push_back(static_cast<char>(b));
        }
    } catch (const Memory::AccessViolation&) {
        out = "<invalid_guest_ptr>";
    }
    return out;
}

} // namespace

// 0x80395A24 -> PPCHalt
extern "C" void PPCHalt_80395A24()
{
    // A halt is never a clean shutdown: the guest reaches it after OSPanic or an
    // unrecoverable OS error. Report it like every other fatal path so the run
    // folder gets the same artifact set, and exit non-zero.
    RT_LOGF(RT_TAG_OS, "PPCHalt called, exiting process\n");
    std::fflush(stderr);
    RuntimeCrash::WriteCrashArtifacts("halt", "The guest executed PPCHalt.");
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup("the guest operating system halted the console",
                          "The game executed PPCHalt, which the console only reaches after an "
                          "unrecoverable error.");
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

PPC_NATIVE_OVERRIDE_VOID(80395A24, PPCHalt_80395A24, (), ());

// 0x803B5C74 -> OS::Panic
extern "C" void OS__Panic_803B5C74_Cpu(CpuContext* ctx)
{
    const uint32_t file_ptr = ctx ? ctx->gpr[3] : 0;
    const int line = ctx ? static_cast<int>(ctx->gpr[4]) : 0;
    const uint32_t fmt_ptr = ctx ? ctx->gpr[5] : 0;

    const std::string file = ReadGuestStringSafe(file_ptr);
    const std::string fmt = ReadGuestStringSafe(fmt_ptr);

    std::fprintf(stderr, "\n\n");
    std::fprintf(stderr, "========================================\n");
    RT_LOGF(RT_TAG_OS, "OS::Panic called!\n");
    std::fprintf(stderr, "========================================\n");
    RT_LOGF(RT_TAG_OS, "Location: %s:%d\n", file.empty() ? "<null>" : file.c_str(), line);
    if (!fmt.empty()) {
        RT_LOGF(RT_TAG_OS, "Message: %s\n", fmt.c_str());
    }
    std::fprintf(stderr, "========================================\n");
    std::fflush(stderr);
    
    RT_LOGF(RT_TAG_OS, "Dumping CPU state...\n");
    std::fflush(stderr);
    
    
    RT_LOGF(RT_TAG_OS, "CPU registers:\n");
    if (ctx) {
        std::fprintf(stderr, "  PC: 0x%08X  LR: 0x%08X  CTR: 0x%08X\n", ctx->pc, ctx->lr, ctx->ctr);
        std::fprintf(stderr, "  r1: 0x%08X  r2: 0x%08X  r13: 0x%08X\n", ctx->gpr[1], ctx->gpr[2], ctx->gpr[13]);
        std::fprintf(stderr, "  r3: 0x%08X  r4: 0x%08X  r5: 0x%08X\n", ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
    } else {
        std::fprintf(stderr, "  (CPU context is NULL)\n");
    }
    
    std::fprintf(stderr, "========================================\n");
    RT_LOGF(RT_TAG_OS, "Flushing output and exiting...\n");
    std::fflush(stderr);
    std::fflush(stdout);

    std::string details = "The game reported an OS panic";
    if (!file.empty()) {
        details += " at ";
        details += file;
        details += ":";
        details += std::to_string(line);
    }
    if (!fmt.empty()) {
        details += "\n\n";
        details += fmt;
    }
    RuntimeCrash::WriteCrashArtifacts("panic", details);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup("the guest operating system reported a panic", details);
    MarkFatalErrorReported();

    // Give extra time for output to flush with PowerShell redirection
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    std::exit(EXIT_FAILURE);
}

PPC_NATIVE_OVERRIDE_VOID(803B5C74, OS__Panic_803B5C74_Cpu, (CpuContext* ctx), (ctx));

namespace {
// Leaving the game the way the console would leave it: a clean exit.
[[noreturn]] void ExitCleanly(const char* why)
{
    RT_LOGF(RT_TAG_OS, "%s: closing the game\n", why);
    std::fflush(stderr);
    // The AX mix worker holds resolved host pointers into the guest regions;
    // it must be stopped before those mappings are torn down.
    AxDspHle::ShutdownMixWorker();
    Memory::Reset();
    SetRuntimeExitCode(0);
    std::exit(EXIT_SUCCESS);
}
} // namespace

// 0x803BAB98 -> OSResetSystem
extern "C" uint32_t OSResetSystem()
{
    ExitCleanly("OSResetSystem (console reset)");
}

PPC_NATIVE_OVERRIDE(803BAB98, OSResetSystem, uint32_t, (), ());

// The HOME Menu's exits (Strikers Charged's ResetTask fades out, calls one of these and spins
// forever). On the console they launch the Wii Menu, reboot, power off or restart the disc through
// IOS, none of which exists here, so each one closes the game. A relaunch for Reset is not done.
// 0x803BAA28 -> OSReturnToMenu
extern "C" void MSC_OSReturnToMenu_803BAA28()
{
    ExitCleanly("OSReturnToMenu (HOME Menu: Wii Menu)");
}
PPC_NATIVE_OVERRIDE_VOID(803BAA28, MSC_OSReturnToMenu_803BAA28, (), ());

// 0x803BAAFC -> __OSReturnToMenuForError
extern "C" void MSC_OSReturnToMenuForError_803BAAFC()
{
    ExitCleanly("__OSReturnToMenuForError");
}
PPC_NATIVE_OVERRIDE_VOID(803BAAFC, MSC_OSReturnToMenuForError_803BAAFC, (), ());

// 0x803BA99C -> OSRestart(u32 resetCode)
extern "C" void MSC_OSRestart_803BA99C(uint32_t resetCode)
{
    RT_LOGF(RT_TAG_OS, "OSRestart(0x%08X)\n", resetCode);
    ExitCleanly("OSRestart (HOME Menu: Reset)");
}
PPC_NATIVE_OVERRIDE_VOID(803BA99C, MSC_OSRestart_803BA99C, (uint32_t resetCode), (resetCode));

// 0x803BA730 -> OSRebootSystem
extern "C" void MSC_OSRebootSystem_803BA730()
{
    ExitCleanly("OSRebootSystem");
}
PPC_NATIVE_OVERRIDE_VOID(803BA730, MSC_OSRebootSystem_803BA730, (), ());

// 0x803BA7D8 -> OSShutdownSystem
extern "C" void MSC_OSShutdownSystem_803BA7D8()
{
    ExitCleanly("OSShutdownSystem (power off)");
}
PPC_NATIVE_OVERRIDE_VOID(803BA7D8, MSC_OSShutdownSystem_803BA7D8, (), ());

// 0x803BE8D0 -> exit(int status)
extern "C" uint32_t Exit_803BE8D0(int status)
{
    RT_LOGF(RT_TAG_OS, "exit(status=%d) called via 0x803BE8D0\n", status);
    std::fflush(stderr);
    if (status != 0) {
        const std::string details =
            "The game called exit(" + std::to_string(status) + ").";
        RuntimeCrash::WriteCrashArtifacts("exit", details);
        ShowRuntimeFatalPopup("the game exited with an error status", details);
        MarkFatalErrorReported();
    }
    SetRuntimeExitCode(status);
    std::exit(status);
    return static_cast<uint32_t>(status);
}

PPC_NATIVE_OVERRIDE(803BE8D0, Exit_803BE8D0, uint32_t, (int status), (status));
