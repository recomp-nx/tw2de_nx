#include "profiler/switch_cpu_profiler.h"

#include <switch.h>

#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The executable's first instruction (libnx crt0), address 0 of the ELF: code offsets are relative to it.
// (__start__, used before, is an absolute linker symbol that is never relocated: it gave a base of 0, so
// every sample fell outside the module and was dropped.)
// tw2de (2026-10-30): declared as the function it is (as sdk core/exception_handler_switch.cpp does): declared as a
// char, the LTO link refused it ("function '_start' redeclared as variable").
extern "C" void _start(void);

namespace
{
    constexpr int MAX_THREADS = 96; // tw2de: ReXGlue + Mesa run more threads than Unleashed
    constexpr uint64_t SAMPLE_INTERVAL_NS = 2'000'000;   // 500 samples per second per thread
    constexpr uint64_t REPORT_INTERVAL_NS = 30'000'000'000ull;
    constexpr uint32_t TABLE_SIZE = 1u << 17;            // (thread, 16-byte code line) -> samples
    // tw2de (as futari-nx): enough lines that a busy thread's tail is not cut off, each with its raw sample count, so
    // tools/switch-cpu-profile.py can add a function's lines up into its self time.
    constexpr uint32_t REPORT_LINES_PER_THREAD = 4000;
    constexpr uint32_t REPORT_MIN_SAMPLES = 50;         // threads that ran less are listed without lines

    // Samples taken while a thread sat in a system call (waits, sleeps, IPC): where it came from, as its
    // return address (LR) and the first return addresses found on its stack, so that a wait can be traced
    // to the code that waits (tools/switch-cpu-profile.py names them).
    constexpr uint32_t WAIT_TABLE_SIZE = 1u << 12;
    constexpr uint32_t WAIT_CALLERS = 16;                // LR + 15 return addresses from the stack (round 12: was 4;
                                                         // 2026-10-24: was 8, which the host frames of a guest wait
                                                         // used up before the game function that waited)
    constexpr uint32_t WAIT_STACK_BYTES = 4096;          // scanned above SP, while the thread is paused (was 1024)
    constexpr uint32_t REPORT_WAIT_LINES_PER_THREAD = 12;

    // Round 12, SwitchSlowFrameProfileMs: every sample of the game thread, running or waiting, with its callers, for the
    // last two seconds; the ones inside a frame whose work took at least the threshold go to their own tables.
    constexpr uint32_t FRAME_SAMPLES = 1024;
    constexpr uint32_t SLOW_FRAME_RECORDS = 64;
    constexpr uint32_t SLOW_TABLE_SIZE = 1u << 12;
    constexpr uint32_t REPORT_SLOW_LINES = 60;
    constexpr uint32_t REPORT_SLOW_STACKS = 40;

    struct ThreadSlot
    {
        Handle handle = 0;
        char name[48]{};
        bool active = false;
        uint64_t usageTicks = 0;    // CPU ticks at the previous AppendThreadCpuUsage
        uint64_t sampleTicks = 0;   // CPU ticks at the previous sample
        uint32_t samples = 0;       // samples taken while it had run since the previous one
        uint32_t idleSamples = 0;   // samples taken while it had not run
        uint32_t syscallSamples = 0;
        uint64_t coreTicks[4]{};    // CPU ticks on each core at the previous AppendThreadCpuUsage
    };

    struct Bucket
    {
        uint32_t key; // 0 = empty; otherwise ((slot << 26) | (offset >> 4)) + 1
        uint32_t count;
    };

    struct WaitBucket
    {
        uint32_t slot;
        uint32_t count;             // 0 = empty
        uint32_t callers[WAIT_CALLERS]; // module offsets (0 = none)
    };

    // A sample of the game thread for the slow-frame report: where it was (the code offset, or 0) and the return
    // addresses found on its stack (0 = none), and when.
    struct FrameSample
    {
        uint64_t tick;
        uint32_t chain[WAIT_CALLERS];
        uint32_t pc;
        bool running;
    };

    struct SlowFrameRecord
    {
        uint64_t start;
        uint64_t end;
    };

    struct StackBucket
    {
        uint32_t count;             // 0 = empty
        uint32_t chain[WAIT_CALLERS + 1]; // the code offset, then the callers
    };

    ThreadSlot g_slots[MAX_THREADS];
    Mutex g_slotsMutex;

    // SwitchSlowFrameProfileMs. The records are written by the game thread (Video::Present) and read by the sampler; the
    // samples and the tables only by the sampler, the tables under g_slotsMutex.
    std::atomic<uint64_t> g_slowThresholdTicks{ 0 };
    std::atomic<uint32_t> g_frameThread{ 0 };
    // Round 14: the thread registered as "main" (the game's main thread); only its frames are timed.
    std::atomic<uint32_t> g_mainThread{ 0 };
    uint64_t g_frameWorkStart = 0;
    std::atomic<uint32_t> g_frames{ 0 };
    SlowFrameRecord g_slowFrameRecords[SLOW_FRAME_RECORDS];
    std::atomic<uint32_t> g_slowFrameWrite{ 0 };
    uint32_t g_slowFrameRead = 0;
    // tw2de (2026-10-24): the samples and tables of a slow frame, for the game thread (set 0) and for the thread
    // marked as its companion (set 1: the renderer's render thread, MarkSlowFrameCompanion): what each did inside the
    // same slow frames.
    struct SlowSet
    {
        FrameSample frameSamples[FRAME_SAMPLES];
        uint64_t frameSamplesWritten = 0;
        Bucket table[SLOW_TABLE_SIZE];
        StackBucket stacks[SLOW_TABLE_SIZE];
        uint32_t samples = 0;
        uint32_t runningSamples = 0;
    };
    SlowSet g_slowSets[2];
    std::atomic<uint32_t> g_companionThread{ 0 };
    uint32_t g_slowFrames = 0;
    uint64_t g_slowFrameTicks = 0;
    uint64_t g_slowLongestTicks = 0;
    uint32_t g_slowDropped = 0;
    uint64_t g_usageWindowStart = 0;

    Bucket g_table[TABLE_SIZE];
    uint32_t g_tableDropped = 0;

    // tw2de (2026-10-25, tw2de_profile_inclusive): the main thread's call chains while it runs game code: every
    // return address on its stack (deduplicated within a sample) counted once per sample, so a function's count is the
    // samples it was on the stack for (its inclusive time). The flat table above says where the time is spent; this
    // says under which caller. Read while the thread is paused, like the wait stacks, but deeper.
    constexpr uint32_t INCL_TABLE_SIZE = 1u << 14;
    constexpr uint32_t INCL_CALLERS = 96;
    constexpr uint32_t INCL_STACK_BYTES = 24 * 1024;
    constexpr uint32_t REPORT_INCL_LINES = 600;
    std::atomic<bool> g_inclusiveOn{ false };
    Bucket g_inclTable[INCL_TABLE_SIZE];
    uint32_t g_inclSamples = 0;
    uint32_t g_inclDropped = 0;
    WaitBucket g_waitTable[WAIT_TABLE_SIZE];
    uint32_t g_waitDropped = 0;
    Thread g_samplerThread;
    bool g_samplerStarted = false;

    uint64_t ThreadTicks(Handle handle)
    {
        uint64_t ticks = 0;
        if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, handle, UINT64_MAX)))
            return 0;
        return ticks;
    }

    uint64_t g_moduleBase = 0;
    uint64_t g_moduleSize = 0;

    void FindModule()
    {
        if (g_moduleBase != 0)
            return;

        g_moduleBase = reinterpret_cast<uint64_t>(&_start);

        // The size of the code mapping that holds it, for the report.
        MemoryInfo info{};
        u32 pageInfo = 0;
        if (R_SUCCEEDED(svcQueryMemory(&info, &pageInfo, g_moduleBase)))
            g_moduleSize = info.addr + info.size - g_moduleBase;
    }

    bool InModule(uint64_t address)
    {
        return address >= g_moduleBase && address - g_moduleBase < g_moduleSize;
    }

    uint32_t ReadCode(uint64_t address)
    {
        uint32_t instruction;
        memcpy(&instruction, reinterpret_cast<const void*>(address), sizeof(instruction));
        return instruction;
    }

    // A return address: inside the code and right after a BL or BLR.
    bool IsReturnAddress(uint64_t address)
    {
        if ((address & 3) != 0 || !InModule(address) || address - g_moduleBase < 4)
            return false;

        const uint32_t previous = ReadCode(address - 4);
        return (previous & 0xFC000000u) == 0x94000000u || (previous & 0xFFFFFC1Fu) == 0xD63F0000u;
    }

    // The thread is in a system call: Horizon reports a thread blocked in one with its PC at the SVC instruction
    // itself (round 9: the check only looked right after it, so no wait was ever attributed), and one stopped
    // on its way out right after it.
    bool InSystemCall(uint64_t pc)
    {
        if (!InModule(pc) || (pc & 3) != 0)
            return false;

        if ((ReadCode(pc) & 0xFFE0001Fu) == 0xD4000001u)
            return true;

        return pc - g_moduleBase >= 4 && (ReadCode(pc - 4) & 0xFFE0001Fu) == 0xD4000001u;
    }

    void AddSample(uint32_t slot, uint64_t pc)
    {
        const uint64_t base = g_moduleBase;
        if (pc < base || pc - base >= (uint64_t(1) << 30))
            return;

        const uint32_t key = ((slot << 26) | uint32_t((pc - base) >> 4)) + 1;
        uint32_t index = (key * 2654435761u) & (TABLE_SIZE - 1);
        for (uint32_t probe = 0; probe < 64; probe++)
        {
            Bucket& bucket = g_table[(index + probe) & (TABLE_SIZE - 1)];
            if (bucket.key == key)
            {
                bucket.count++;
                return;
            }
            if (bucket.key == 0)
            {
                bucket.key = key;
                bucket.count = 1;
                return;
            }
        }

        g_tableDropped++;
    }

    void AddInclusive(uint32_t offset)
    {
        const uint32_t key = (offset >> 2) + 1;  // the exact return address (4-byte aligned)
        uint32_t index = (key * 2654435761u) & (INCL_TABLE_SIZE - 1);
        for (uint32_t probe = 0; probe < 64; probe++)
        {
            Bucket& bucket = g_inclTable[(index + probe) & (INCL_TABLE_SIZE - 1)];
            if (bucket.key == key)
            {
                bucket.count++;
                return;
            }
            if (bucket.key == 0)
            {
                bucket.key = key;
                bucket.count = 1;
                return;
            }
        }
        g_inclDropped++;
    }

    // Every return address above SP (up to INCL_STACK_BYTES, INCL_CALLERS of them) and the LR, once each.
    uint32_t DeepCallers(const ThreadContext& context, uint32_t* callers, uint8_t* stackCopy)
    {
        uint32_t count = 0;
        auto add = [&](uint64_t value) {
            const uint32_t offset = uint32_t(value - g_moduleBase);
            for (uint32_t i = 0; i < count; i++)
                if (callers[i] == offset)
                    return;
            if (count < INCL_CALLERS)
                callers[count++] = offset;
        };
        if (IsReturnAddress(context.lr))
            add(context.lr);
        MemoryInfo info{};
        u32 pageInfo = 0;
        const uint64_t sp = context.sp;
        if (R_SUCCEEDED(svcQueryMemory(&info, &pageInfo, sp)) && info.type != MemType_Unmapped && (info.perm & Perm_R) != 0)
        {
            const uint64_t end = std::min<uint64_t>(info.addr + info.size, sp + INCL_STACK_BYTES);
            if (end > sp)
            {
                memcpy(stackCopy, reinterpret_cast<const void*>(sp), size_t(end - sp));
                for (uint64_t offset = 0; offset + 8 <= end - sp && count < INCL_CALLERS; offset += 8)
                {
                    uint64_t value;
                    memcpy(&value, stackCopy + offset, sizeof(value));
                    if (IsReturnAddress(value))
                        add(value);
                }
            }
        }
        return count;
    }

    void AddWaitSample(uint32_t slot, const uint32_t (&callers)[WAIT_CALLERS])
    {
        uint32_t hash = slot * 2654435761u;
        for (uint32_t caller : callers)
            hash = (hash ^ caller) * 2654435761u;

        for (uint32_t probe = 0; probe < 32; probe++)
        {
            WaitBucket& bucket = g_waitTable[(hash + probe) & (WAIT_TABLE_SIZE - 1)];
            if (bucket.count != 0 && bucket.slot == slot && memcmp(bucket.callers, callers, sizeof(callers)) == 0)
            {
                bucket.count++;
                return;
            }
            if (bucket.count == 0)
            {
                bucket.slot = slot;
                bucket.count = 1;
                memcpy(bucket.callers, callers, sizeof(callers));
                return;
            }
        }

        g_waitDropped++;
    }

    // The return addresses on a paused thread's stack (and its LR first), up to `capacity`.
    uint32_t StackCallers(const ThreadContext& context, uint32_t* callers, uint32_t capacity, uint8_t* stackCopy)
    {
        uint32_t count = 0;
        if (IsReturnAddress(context.lr))
            callers[count++] = uint32_t(context.lr - g_moduleBase);

        MemoryInfo info{};
        u32 pageInfo = 0;
        const uint64_t sp = context.sp;
        if (count < capacity && R_SUCCEEDED(svcQueryMemory(&info, &pageInfo, sp)) && info.type != MemType_Unmapped &&
            (info.perm & Perm_R) != 0)
        {
            const uint64_t end = std::min<uint64_t>(info.addr + info.size, sp + WAIT_STACK_BYTES);
            if (end > sp)
            {
                memcpy(stackCopy, reinterpret_cast<const void*>(sp), size_t(end - sp));
                for (uint64_t offset = 0; offset + 8 <= end - sp && count < capacity; offset += 8)
                {
                    uint64_t value;
                    memcpy(&value, stackCopy + offset, sizeof(value));
                    if (IsReturnAddress(value) && uint32_t(value - g_moduleBase) != callers[count ? count - 1 : 0])
                        callers[count++] = uint32_t(value - g_moduleBase);
                }
            }
        }

        return count;
    }

    void AddSlowSample(SlowSet& set, const FrameSample& sample)
    {
        // Its code line, as a pseudo-thread of slot 0 (the table holds only this thread).
        if (sample.pc != 0)
        {
            const uint32_t key = (sample.pc >> 4) + 1;
            uint32_t index = (key * 2654435761u) & (SLOW_TABLE_SIZE - 1);
            bool added = false;
            for (uint32_t probe = 0; probe < 64 && !added; probe++)
            {
                Bucket& bucket = set.table[(index + probe) & (SLOW_TABLE_SIZE - 1)];
                if (bucket.key == key || bucket.key == 0)
                {
                    bucket.key = key;
                    bucket.count++;
                    added = true;
                }
            }
            g_slowDropped += !added;
        }

        // Its call chain: the code offset, then the callers.
        uint32_t chain[WAIT_CALLERS + 1]{};
        chain[0] = sample.pc;
        memcpy(chain + 1, sample.chain, sizeof(sample.chain));
        uint32_t hash = 0;
        for (uint32_t value : chain)
            hash = (hash ^ value) * 2654435761u;
        for (uint32_t probe = 0; probe < 32; probe++)
        {
            StackBucket& bucket = set.stacks[(hash + probe) & (SLOW_TABLE_SIZE - 1)];
            if (bucket.count != 0 && memcmp(bucket.chain, chain, sizeof(chain)) == 0)
            {
                bucket.count++;
                break;
            }
            if (bucket.count == 0)
            {
                bucket.count = 1;
                memcpy(bucket.chain, chain, sizeof(chain));
                break;
            }
        }

        set.samples++;
        set.runningSamples += sample.running;
    }

    // The slow frames whose samples have all been taken (the newest sample is past their end): their samples go to the
    // tables. Under g_slotsMutex.
    void ProcessSlowFrames()
    {
        const SlowSet& game = g_slowSets[0];
        if (game.frameSamplesWritten == 0)
            return;

        const uint64_t newest = game.frameSamples[(game.frameSamplesWritten - 1) % FRAME_SAMPLES].tick;
        const uint32_t written = g_slowFrameWrite.load(std::memory_order_acquire);
        if (written - g_slowFrameRead > SLOW_FRAME_RECORDS)
            g_slowFrameRead = written - SLOW_FRAME_RECORDS;

        while (g_slowFrameRead != written)
        {
            const SlowFrameRecord record = g_slowFrameRecords[g_slowFrameRead % SLOW_FRAME_RECORDS];
            if (record.end > newest)
                break;

            for (SlowSet& set : g_slowSets)
            {
                const uint64_t available = std::min<uint64_t>(set.frameSamplesWritten, FRAME_SAMPLES);
                for (uint64_t i = set.frameSamplesWritten - available; i < set.frameSamplesWritten; i++)
                {
                    const FrameSample& sample = set.frameSamples[i % FRAME_SAMPLES];
                    if (sample.tick >= record.start && sample.tick < record.end)
                        AddSlowSample(set, sample);
                }
            }

            const uint64_t ticks = record.end - record.start;
            g_slowFrames++;
            g_slowFrameTicks += ticks;
            g_slowLongestTicks = std::max(g_slowLongestTicks, ticks);
            g_slowFrameRead++;
        }
    }

    // Report data copied out of the tables under the lock; formatted and written without it.
    struct ReportThread
    {
        uint32_t slot;
        char name[48];
        uint32_t samples;
        uint32_t idleSamples;
        uint32_t syscallSamples;
    };

    struct ReportSlowFrames
    {
        uint32_t frames;            // all frames of the window
        uint32_t slowFrames;
        uint64_t slowTicks;
        uint64_t longestTicks;
        uint32_t samples;
        uint32_t running;
        std::vector<Bucket> lines;
        std::vector<StackBucket> stacks;
        // tw2de (2026-10-24): the companion thread's (the render thread) in the same frames.
        uint32_t companionSamples = 0;
        std::vector<Bucket> companionLines;
        std::vector<StackBucket> companionStacks;
    };

    // One thread's samples in the slow frames, in the format of a thread (tools/switch-cpu-profile.py names it).
    void FormatSlowThread(std::string& report, const char* name, uint32_t samples, std::vector<Bucket>& lines,
        std::vector<StackBucket>& stacks)
    {
        char line[256];
        snprintf(line, sizeof(line), "  thread %s in slow frames: %u samples while running, 0 while waiting\n", name, samples);
        report += line;
        std::sort(lines.begin(), lines.end(), [](const Bucket& a, const Bucket& b) { return a.count > b.count; });
        uint32_t shown = 0;
        for (const Bucket& bucket : lines)
        {
            if (shown++ == REPORT_SLOW_LINES)
                break;
            snprintf(line, sizeof(line), "    %6.3f%%  +0x%07x\n", 100.0 * bucket.count / samples, (bucket.key - 1) << 4);
            report += line;
        }
        snprintf(line, sizeof(line), "    system calls in %u samples; from (return address, then stack):\n", samples);
        report += line;
        std::sort(stacks.begin(), stacks.end(), [](const StackBucket& a, const StackBucket& b) { return a.count > b.count; });
        shown = 0;
        for (const StackBucket& bucket : stacks)
        {
            if (shown++ == REPORT_SLOW_STACKS)
                break;
            int length = snprintf(line, sizeof(line), "      %5.1f%%  wait", 100.0 * bucket.count / samples);
            for (uint32_t caller : bucket.chain)
            {
                if (caller != 0 && length > 0 && size_t(length) < sizeof(line) - 16)
                    length += snprintf(line + length, sizeof(line) - length, " +0x%07x", caller);
            }
            report += line;
            report += '\n';
        }
    }

    void FormatReport(std::string& report, uint64_t windowTicks, const std::vector<ReportThread>& threads,
        std::vector<Bucket>& buckets, std::vector<WaitBucket>& waits, uint32_t dropped, uint32_t waitDropped,
        ReportSlowFrames& slow)
    {
        report.reserve(65536);

        char line[256];
        snprintf(line, sizeof(line), "[cpu profile] %.1f s, module base 0x%llx (code 0x%llx bytes), %u samples dropped; "
            "run tools/switch-cpu-profile.py on this log to name the addresses\n",
            double(windowTicks) / double(armGetSystemTickFreq()), (unsigned long long)g_moduleBase,
            (unsigned long long)g_moduleSize, dropped);
        report += line;

        std::sort(buckets.begin(), buckets.end(), [](const Bucket& a, const Bucket& b)
            {
                const uint32_t slotA = (a.key - 1) >> 26;
                const uint32_t slotB = (b.key - 1) >> 26;
                return slotA != slotB ? slotA < slotB : a.count > b.count;
            });

        std::sort(waits.begin(), waits.end(), [](const WaitBucket& a, const WaitBucket& b)
            {
                return a.slot != b.slot ? a.slot < b.slot : a.count > b.count;
            });

        for (const ReportThread& thread : threads)
        {
            snprintf(line, sizeof(line), "  thread %s: %u samples while running, %u while waiting\n",
                thread.name, thread.samples, thread.idleSamples);
            report += line;

            if (thread.samples >= REPORT_MIN_SAMPLES)
            {
                uint32_t shown = 0;
                for (const Bucket& bucket : buckets)
                {
                    if (((bucket.key - 1) >> 26) != thread.slot || shown++ == REPORT_LINES_PER_THREAD)
                        continue;

                    snprintf(line, sizeof(line), "    %6.3f%%  +0x%07x x%u\n", 100.0 * bucket.count / thread.samples,
                        ((bucket.key - 1) & ((1u << 26) - 1)) << 4, bucket.count);
                    report += line;
                }
            }

            // Where its system calls (waits, sleeps, IPC) came from.
            if (thread.syscallSamples >= REPORT_MIN_SAMPLES / 5)
            {
                snprintf(line, sizeof(line), "    system calls in %u samples; from (return address, then stack):\n",
                    thread.syscallSamples);
                report += line;

                uint32_t shown = 0;
                for (const WaitBucket& wait : waits)
                {
                    if (wait.slot != thread.slot || shown++ == REPORT_WAIT_LINES_PER_THREAD)
                        continue;

                    int length = snprintf(line, sizeof(line), "      %5.1f%%  wait", 100.0 * wait.count / thread.syscallSamples);
                    for (uint32_t caller : wait.callers)
                    {
                        if (caller != 0 && length > 0 && size_t(length) < sizeof(line) - 16)
                            length += snprintf(line + length, sizeof(line) - length, " +0x%07x", caller);
                    }
                    report += line;
                    report += '\n';
                }
            }
        }

        if (waitDropped != 0)
        {
            snprintf(line, sizeof(line), "  %u system call samples dropped (table full)\n", waitDropped);
            report += line;
        }

        const uint64_t threshold = g_slowThresholdTicks.load(std::memory_order_relaxed);
        if (threshold != 0)
        {
            const double tickMs = 1000.0 / double(armGetSystemTickFreq());
            snprintf(line, sizeof(line), "  slow frames: %u of %u frames with at least %.0f ms of game-thread work (longest %.1f ms, "
                "%.0f ms in all); every sample of the game thread in them, running or waiting, and the callers on its stack:\n",
                slow.slowFrames, slow.frames, double(threshold) * tickMs, double(slow.longestTicks) * tickMs,
                double(slow.slowTicks) * tickMs);
            report += line;

            // In the format of a thread, so that tools/switch-cpu-profile.py names it (its "system calls" are every
            // sample here: the call chains of all of them).
            if (slow.samples != 0)
                FormatSlowThread(report, "main", slow.samples, slow.lines, slow.stacks);
            if (slow.companionSamples != 0)
                FormatSlowThread(report, "render", slow.companionSamples, slow.companionLines, slow.companionStacks);
        }
    }

    void SamplerThread(void*)
    {
        const Handle self = threadGetCurHandle();
        uint64_t reportStart = armGetSystemTick();
        const uint64_t reportTicks = armNsToTicks(REPORT_INTERVAL_NS);

        std::vector<ReportThread> reportThreads;
        std::vector<Bucket> reportBuckets;
        std::vector<WaitBucket> reportWaits;
        ReportSlowFrames reportSlow;
        static uint8_t stackCopy[WAIT_STACK_BYTES];
        static uint8_t inclCopy[INCL_STACK_BYTES];
        static uint32_t inclCallers[INCL_CALLERS];

        while (true)
        {
            svcSleepThread(SAMPLE_INTERVAL_NS);

            mutexLock(&g_slotsMutex);
            for (uint32_t slot = 0; slot < MAX_THREADS; slot++)
            {
                ThreadSlot& thread = g_slots[slot];
                if (!thread.active || thread.handle == self)
                    continue;

                // A thread that has not run since the last sample is waiting: its position is where it
                // blocked, which says nothing about where CPU time goes. (The game thread, with the slow-frame
                // report on, is still captured: where it waits is what a slow frame is made of.)
                const bool slowOn = g_slowThresholdTicks.load(std::memory_order_relaxed) != 0;
                const bool companion = slowOn && thread.handle == g_companionThread.load(std::memory_order_relaxed);
                const bool frameThread = (slowOn && thread.handle == g_frameThread.load(std::memory_order_relaxed)) || companion;
                const uint64_t ticks = ThreadTicks(thread.handle);
                const bool ran = ticks != thread.sampleTicks;
                if (!ran)
                {
                    thread.idleSamples++;
                    if (!frameThread)
                        continue;
                }
                thread.sampleTicks = ticks;

                // Nothing between pause and resume may allocate, lock or log: the paused thread may hold
                // any of those locks. Its stack is only read while it is paused (it cannot exit meanwhile).
                if (R_FAILED(svcSetThreadActivity(thread.handle, ThreadActivity_Paused)))
                    continue;

                ThreadContext context;
                const Result result = svcGetThreadContext3(&context, thread.handle);

                bool inSystemCall = false;
                uint32_t callers[WAIT_CALLERS]{};
                if (R_SUCCEEDED(result) && (InSystemCall(context.pc.x) || frameThread))
                {
                    inSystemCall = InSystemCall(context.pc.x);
                    StackCallers(context, callers, WAIT_CALLERS, stackCopy);
                }
                // tw2de (2026-10-25, tw2de_profile_inclusive): the main thread's chain while it runs (not in a
                // system call), read while it is paused.
                uint32_t inclCount = 0;
                const bool inclusive = R_SUCCEEDED(result) && ran && g_inclusiveOn.load(std::memory_order_relaxed) &&
                                       thread.handle == g_mainThread.load(std::memory_order_relaxed) &&
                                       !InSystemCall(context.pc.x);
                if (inclusive)
                    inclCount = DeepCallers(context, inclCallers, inclCopy);

                svcSetThreadActivity(thread.handle, ThreadActivity_Runnable);

                if (R_SUCCEEDED(result))
                {
                    if (frameThread)
                    {
                        SlowSet& set = g_slowSets[companion ? 1 : 0];
                        FrameSample& sample = set.frameSamples[set.frameSamplesWritten++ % FRAME_SAMPLES];
                        sample.tick = armGetSystemTick();
                        sample.pc = InModule(context.pc.x) ? uint32_t(context.pc.x - g_moduleBase) : 0;
                        memcpy(sample.chain, callers, sizeof(callers));
                        sample.running = ran;
                    }

                    if (inclusive)
                    {
                        g_inclSamples++;
                        if (InModule(context.pc.x))
                            AddInclusive(uint32_t(context.pc.x - g_moduleBase) & ~3u);
                        for (uint32_t i = 0; i < inclCount; i++)
                            AddInclusive(inclCallers[i]);
                    }
                    if (ran)
                    {
                        AddSample(slot, context.pc.x);
                        thread.samples++;
                        if (inSystemCall)
                        {
                            AddWaitSample(slot, callers);
                            thread.syscallSamples++;
                        }
                    }
                }
            }
            ProcessSlowFrames();
            mutexUnlock(&g_slotsMutex);

            const uint64_t now = armGetSystemTick();
            if (now - reportStart >= reportTicks)
            {
                // Copy and reset under the lock; format and write without it.
                reportThreads.clear();
                reportBuckets.clear();
                reportWaits.clear();

                mutexLock(&g_slotsMutex);
                for (uint32_t slot = 0; slot < MAX_THREADS; slot++)
                {
                    ThreadSlot& thread = g_slots[slot];
                    if (thread.samples != 0)
                    {
                        ReportThread copy{};
                        copy.slot = slot;
                        memcpy(copy.name, thread.name, sizeof(copy.name));
                        copy.samples = thread.samples;
                        copy.idleSamples = thread.idleSamples;
                        copy.syscallSamples = thread.syscallSamples;
                        reportThreads.push_back(copy);
                    }

                    thread.samples = 0;
                    thread.idleSamples = 0;
                    thread.syscallSamples = 0;
                }

                for (Bucket& bucket : g_table)
                {
                    if (bucket.key != 0)
                        reportBuckets.push_back(bucket);
                }
                for (WaitBucket& wait : g_waitTable)
                {
                    if (wait.count != 0)
                        reportWaits.push_back(wait);
                }

                std::vector<Bucket> reportIncl;
                for (const Bucket& bucket : g_inclTable)
                {
                    if (bucket.key != 0)
                        reportIncl.push_back(bucket);
                }
                const uint32_t inclSamples = g_inclSamples;
                const uint32_t inclDropped = g_inclDropped;
                memset(g_inclTable, 0, sizeof(g_inclTable));
                g_inclSamples = 0;
                g_inclDropped = 0;
                const uint32_t dropped = g_tableDropped;
                const uint32_t waitDropped = g_waitDropped;
                memset(g_table, 0, sizeof(g_table));
                memset(g_waitTable, 0, sizeof(g_waitTable));
                g_tableDropped = 0;
                g_waitDropped = 0;

                reportSlow.lines.clear();
                reportSlow.stacks.clear();
                reportSlow.companionLines.clear();
                reportSlow.companionStacks.clear();
                for (const Bucket& bucket : g_slowSets[0].table)
                {
                    if (bucket.key != 0)
                        reportSlow.lines.push_back(bucket);
                }
                for (const StackBucket& bucket : g_slowSets[0].stacks)
                {
                    if (bucket.count != 0)
                        reportSlow.stacks.push_back(bucket);
                }
                for (const Bucket& bucket : g_slowSets[1].table)
                {
                    if (bucket.key != 0)
                        reportSlow.companionLines.push_back(bucket);
                }
                for (const StackBucket& bucket : g_slowSets[1].stacks)
                {
                    if (bucket.count != 0)
                        reportSlow.companionStacks.push_back(bucket);
                }
                reportSlow.companionSamples = g_slowSets[1].samples;
                reportSlow.frames = g_frames.exchange(0, std::memory_order_relaxed);
                reportSlow.slowFrames = g_slowFrames;
                reportSlow.slowTicks = g_slowFrameTicks;
                reportSlow.longestTicks = g_slowLongestTicks;
                reportSlow.samples = g_slowSets[0].samples;
                reportSlow.running = g_slowSets[0].runningSamples;
                for (SlowSet& set : g_slowSets)
                {
                    memset(set.table, 0, sizeof(set.table));
                    memset(set.stacks, 0, sizeof(set.stacks));
                    set.samples = 0;
                    set.runningSamples = 0;
                }
                g_slowFrames = 0;
                g_slowFrameTicks = 0;
                g_slowLongestTicks = 0;
                mutexUnlock(&g_slotsMutex);

                std::string report;
                FormatReport(report, now - reportStart, reportThreads, reportBuckets, reportWaits, dropped, waitDropped,
                    reportSlow);
                // tw2de (2026-10-24): each thread's CPU time over the report (the kernel's tick count, per core):
                // the samples above say where a thread was when it ran at all in 2 ms, not how long it ran (a thread
                // that wakes every millisecond counted as running 99% of the time while waiting).
                std::string usage;
                os::switch_cpu_profiler::AppendThreadCpuUsage(usage);
                if (!usage.empty())
                    report += "  cpu time (% of one core, by core): " + usage + "\n";
                if (inclSamples != 0)
                {
                    // tw2de (2026-10-25): the main thread's return addresses by samples on the stack (tools/
                    // switch-cpu-profile.py --inclusive names and adds them up by function).
                    std::sort(reportIncl.begin(), reportIncl.end(),
                              [](const Bucket& a, const Bucket& b) { return a.count > b.count; });
                    char line[96];
                    snprintf(line, sizeof(line), "  inclusive main: %u samples running game code, %u dropped\n",
                             inclSamples, inclDropped);
                    report += line;
                    for (size_t i = 0; i < reportIncl.size() && i < REPORT_INCL_LINES; i++)
                    {
                        snprintf(line, sizeof(line), "    incl +0x%07x x%u\n", (reportIncl[i].key - 1) << 2,
                                 reportIncl[i].count);
                        report += line;
                    }
                }
                os::switch_cpu_profiler::WriteLog(report);

                reportStart = armGetSystemTick();
            }
        }
    }

    int FindSlot(Handle handle)
    {
        for (int i = 0; i < MAX_THREADS; i++)
        {
            if (g_slots[i].active && g_slots[i].handle == handle)
                return i;
        }
        return -1;
    }

    struct MutexInitializer
    {
        MutexInitializer() { mutexInit(&g_slotsMutex); }
    } g_mutexInitializer;
}

void os::switch_cpu_profiler::RegisterCurrentThread(const char* name)
{
    RegisterThread(threadGetCurHandle(), name);
}

// tw2de: threads are registered from the SDK's threadCreate / pthread_setname_np wrappers
// (sdk/src/ui/switch_perf.cpp), so every ReXGlue, guest and Mesa thread is covered without
// touching each thread's entry point. A second call for the same handle only renames it.
void os::switch_cpu_profiler::RegisterThread(uint32_t handle, const char* name)
{
    if (handle == 0 || name == nullptr)
        return;

    mutexLock(&g_slotsMutex);
    int slot = FindSlot(handle);
    if (slot < 0)
    {
        for (int i = 0; i < MAX_THREADS; i++)
        {
            if (!g_slots[i].active && g_slots[i].samples == 0)
            {
                slot = i;
                break;
            }
        }
    }

    if (slot >= 0)
    {
        ThreadSlot& thread = g_slots[slot];
        thread.handle = handle;
        snprintf(thread.name, sizeof(thread.name), "%s", name);
        if (strcmp(name, "main") == 0)
            g_mainThread.store(handle, std::memory_order_relaxed);
        thread.usageTicks = ThreadTicks(handle);
        thread.sampleTicks = thread.usageTicks;
        thread.active = true;
    }
    mutexUnlock(&g_slotsMutex);
}

void os::switch_cpu_profiler::RegisterCurrentThreadWithAddress(const char* prefix, uint32_t guestAddress)
{
    // Kept per thread: the slot copies it anyway.
    char name[48];
    snprintf(name, sizeof(name), "%s %08X", prefix, guestAddress);
    RegisterCurrentThread(name);
}

void os::switch_cpu_profiler::UnregisterCurrentThread()
{
    UnregisterThread(threadGetCurHandle());
}

void os::switch_cpu_profiler::UnregisterThread(uint32_t handle)
{
    mutexLock(&g_slotsMutex);
    const int slot = FindSlot(handle);
    if (slot >= 0)
        g_slots[slot].active = false; // Its samples, if any, are still reported once.
    mutexUnlock(&g_slotsMutex);
}

void os::switch_cpu_profiler::Start(bool sampler)
{
    g_usageWindowStart = armGetSystemTick();
    FindModule();

    if (!sampler || g_samplerStarted)
        return;

    // Above every thread it samples (the audio pump is 0x20), so a sample is never late behind them.
    if (R_SUCCEEDED(threadCreate(&g_samplerThread, SamplerThread, nullptr, nullptr, 0x10000, 0x1E, -2)))
    {
        UnregisterThread(g_samplerThread.handle); // tw2de: the threadCreate wrapper registered it; never sample itself
        svcSetThreadCoreMask(g_samplerThread.handle, -1, 0x7);
        if (R_SUCCEEDED(threadStart(&g_samplerThread)))
        {
            g_samplerStarted = true;
            os::switch_cpu_profiler::WriteLogF("CPU profiler: sampling registered threads every %llu us, report every %llu s\n",
                (unsigned long long)(SAMPLE_INTERVAL_NS / 1000), (unsigned long long)(REPORT_INTERVAL_NS / 1000000000ull));
        }
    }
}

void os::switch_cpu_profiler::MarkSlowFrameCompanion(const char* name)
{
    // tw2de (2026-10-24): registered (or renamed) and sampled through the game thread's slow frames as well.
    const Handle current = threadGetCurHandle();
    RegisterThread(current, name);
    g_companionThread.store(current, std::memory_order_relaxed);
}

void os::switch_cpu_profiler::SetInclusive(bool on)
{
    g_inclusiveOn.store(on, std::memory_order_relaxed);
}

void os::switch_cpu_profiler::SetSlowFrameThreshold(uint32_t milliseconds)
{
    g_slowThresholdTicks.store(milliseconds == 0 ? 0 : armNsToTicks(uint64_t(milliseconds) * 1000000ull), std::memory_order_relaxed);
}

void os::switch_cpu_profiler::FrameWorkEnd()
{
    const uint64_t threshold = g_slowThresholdTicks.load(std::memory_order_relaxed);
    if (threshold == 0)
        return;

    // Round 13: the thread presenting now, every time. Round 12 kept the first one, a guest thread that presents
    // the boot screens, so the game thread itself was never sampled as the frame thread and no slow frame was
    // ever reported ("0 of 1162 frames" in windows averaging 27 ms of work).
    const Handle current = threadGetCurHandle();

    // Round 14: the loading screens are presented by a loader thread (entry 82DF'A2B8), whose "frames" are mostly sleeps
    // between file reads; round 13's report counted them as the game thread's (30% of its slow-frame samples were
    // that thread's nanosleep). Only the main thread's frames are timed now, so the report is the game thread's.
    const uint32_t mainThread = g_mainThread.load(std::memory_order_relaxed);
    if (mainThread != 0 && current != mainThread)
    {
        g_frameWorkStart = 0;
        return;
    }

    if (g_frameThread.load(std::memory_order_relaxed) != current)
        g_frameThread.store(current, std::memory_order_relaxed);

    g_frames.fetch_add(1, std::memory_order_relaxed);
    const uint64_t now = armGetSystemTick();
    if (g_frameWorkStart != 0 && now - g_frameWorkStart >= threshold)
    {
        const uint32_t index = g_slowFrameWrite.load(std::memory_order_relaxed);
        g_slowFrameRecords[index % SLOW_FRAME_RECORDS] = { g_frameWorkStart, now };
        g_slowFrameWrite.store(index + 1, std::memory_order_release);
    }
}

void os::switch_cpu_profiler::FrameWorkStart()
{
    if (g_slowThresholdTicks.load(std::memory_order_relaxed) == 0)
        return;

    const uint32_t mainThread = g_mainThread.load(std::memory_order_relaxed);
    if (mainThread == 0 || threadGetCurHandle() == mainThread)
        g_frameWorkStart = armGetSystemTick();
}

void os::switch_cpu_profiler::AppendThreadCpuUsage(std::string& out)
{
    // Called from the game's main thread: never wait for the sampler.
    if (!mutexTryLock(&g_slotsMutex))
        return;

    const uint64_t now = armGetSystemTick();
    const uint64_t window = now - g_usageWindowStart;
    g_usageWindowStart = now;
    if (window == 0)
    {
        mutexUnlock(&g_slotsMutex);
        return;
    }

    char entry[96];
    for (ThreadSlot& thread : g_slots)
    {
        if (!thread.active)
            continue;

        const uint64_t ticks = ThreadTicks(thread.handle);
        const uint64_t used = ticks - thread.usageTicks;
        thread.usageTicks = ticks;

        // Round 12: its time on each core (the kernel counts it per core too).
        // Round 13: a core's count can read lower than the time before (round 12 printed the wrapped difference,
        // "c1 14924679665271"), so a negative step counts as nothing, and the split is only printed when it adds
        // up to the thread's total.
        double corePercent[4]{};
        double coreSum = 0.0;
        for (uint32_t core = 0; core < 4; core++)
        {
            uint64_t coreTicks = 0;
            if (R_FAILED(svcGetInfo(&coreTicks, InfoType_ThreadTickCount, thread.handle, core)))
                coreTicks = thread.coreTicks[core];
            const uint64_t step = coreTicks >= thread.coreTicks[core] ? coreTicks - thread.coreTicks[core] : 0;
            corePercent[core] = 100.0 * double(step) / double(window);
            coreSum += corePercent[core];
            thread.coreTicks[core] = coreTicks;
        }

        // Only threads that did something worth a look.
        const double percent = 100.0 * double(used) / double(window);
        if (percent < 0.5)
            continue;

        // tw2de (2026-10-24): its priority and core mask too (who can take a core from whom: Horizon runs a
        // core's highest-priority ready thread, lower numbers first, and slices equal ones in 10 ms).
        s32 priority = -1, preferred = -1;
        u64 affinity = 0;
        if (R_FAILED(svcGetThreadPriority(&priority, thread.handle)))
            priority = -1;
        if (R_FAILED(svcGetThreadCoreMask(&preferred, &affinity, thread.handle)))
            affinity = 0;
        snprintf(entry, sizeof(entry), "%s%s %.1f%% [p%02X m%llX]", out.empty() || out.back() == ' ' ? "" : ", ",
            thread.name, percent, unsigned(priority), (unsigned long long)affinity);
        out += entry;
        if (percent >= 5.0 && std::abs(coreSum - percent) <= std::max(2.0, percent * 0.1))
        {
            int length = 0;
            char cores[64];
            for (uint32_t core = 0; core < 4; core++)
            {
                if (corePercent[core] >= 1.0 && length >= 0 && size_t(length) < sizeof(cores) - 12)
                    length += snprintf(cores + length, sizeof(cores) - length, "%sc%u %.0f", length == 0 ? " (" : ", ", core, corePercent[core]);
            }
            if (length > 0)
            {
                out += cores;
                out += ')';
            }
        }
    }
    mutexUnlock(&g_slotsMutex);
}

size_t os::switch_cpu_profiler::CopyThreads(ThreadInfo* out, size_t capacity)
{
    size_t count = 0;
    mutexLock(&g_slotsMutex);
    for (const ThreadSlot& thread : g_slots)
    {
        if (!thread.active || count == capacity)
            continue;

        out[count].handle = thread.handle;
        memcpy(out[count].name, thread.name, sizeof(out[count].name));
        count++;
    }
    mutexUnlock(&g_slotsMutex);
    return count;
}

bool os::switch_cpu_profiler::TryGetCurrentThreadName(char* out, size_t size)
{
    if (size == 0 || !mutexTryLock(&g_slotsMutex))
        return false;

    const Handle handle = threadGetCurHandle();
    bool found = false;
    for (const ThreadSlot& thread : g_slots)
    {
        if (thread.active && thread.handle == handle)
        {
            snprintf(out, size, "%s", thread.name);
            found = true;
            break;
        }
    }

    mutexUnlock(&g_slotsMutex);
    return found;
}

uint64_t os::switch_cpu_profiler::ModuleBase()
{
    return g_moduleBase;
}

uint64_t os::switch_cpu_profiler::ModuleSize()
{
    return g_moduleSize;
}

// tw2de: Unleashed writes to stderr.log; here the reports get a file of their own in the SDK's log
// folder (<NRO folder>/log/tw2de_profiler.log), opened once and written in one call per report.
extern "C" const char* RexSwitchLogDir(void);

void os::switch_cpu_profiler::WriteLog(const char* text, size_t size)
{
    static Mutex mutex;
    static FILE* file = nullptr;
    static bool tried = false;

    mutexLock(&mutex);
    if (!tried)
    {
        tried = true;
        // tw2de (2026-10-25): log/, and a new file each run ("w"; it was appended to, and a log held
        // several runs).
        std::string path = RexSwitchLogDir();
        path += "tw2de_profiler.log";
        file = fopen(path.c_str(), "w");
    }
    if (file != nullptr)
    {
        fwrite(text, 1, size, file);
        fflush(file);
    }
    mutexUnlock(&mutex);
}

void os::switch_cpu_profiler::WriteLogF(const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length > 0)
        WriteLog(line, std::min(size_t(length), sizeof(line) - 1));
}
