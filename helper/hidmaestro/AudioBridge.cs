// SPDX-License-Identifier: LGPL-3.0-or-later
using System.IO.MemoryMappedFiles;
using System.Text.Json;
using HIDMaestro;

namespace Satellite.HmHelper;

// Controller audio for one composite persona: the pad's own USB-audio
// endpoints joined to satellite's shared-memory rings (speaker and mic on
// every composite, plus the haptics lanes on a DualSense).
//
// Everything here is deliberately mechanical — lane selection, a
// fixed-layout ring write, a ring read, a channel spread — because this
// process has no test harness. All the logic worth testing (rate
// conversion between the persona's endpoints and the pinned wire rate,
// 20 ms windowing, the codec) lives on the satellite side where ctest
// covers it. The rule the rings encode: the ring always speaks the WIRE's
// channel layout (stereo out, mono in) at the PERSONA's sample rate.
//
// The layout constants below mirror
// src/platform/windows/hidmaestro_audio_wire.h, which is the source of
// truth. Satellite maps exactly AudioSectionSize bytes, so a drift here
// fails the map instead of misreading PCM.
internal sealed class AudioBridge : IDisposable
{
    private const int RingSlots = 32;
    private const int RingHeaderSize = 8;
    private const int SlotSeqNoOffset = 0;
    private const int SlotSerialOffset = 4;
    private const int SlotSeqOffset = 8;
    private const int SlotSamplesOffset = 10;
    private const int SlotDataOffset = 12;
    private const int SlotDataCapacity = 4096;
    private const int SlotSampleCapacity = SlotDataCapacity / 2;
    private const int SlotSize = SlotDataOffset + SlotDataCapacity;
    private const long AudioSectionSize = RingHeaderSize + (long)RingSlots * SlotSize;

    private const int WireSpeakerChannels = 2;
    // Channels 3/4 of a DualSense OUT stream: the two voice-coil
    // actuators, left first. Only an endpoint with at least this many
    // channels has them.
    private const int WireHapticChannels = 2;
    private const int HapticSourceChannels = 4;

    // The SDK raises one USB service interval (1 ms) of OUT audio at a
    // time. A doorbell per millisecond would be a thousand cross-process
    // wakeups a second for nothing, since satellite re-windows into 20 ms
    // Opus frames regardless. 240 frames is 5 ms at 48 kHz: 200 wakeups a
    // second, and at most 5 ms of added latency inside a 20 ms window.
    private const int SpeakerFlushFrames = 240;

    // 500 ms safety net behind the doorbell, matching the adapter's ring
    // workers: a missed signal costs latency, never a stuck stream.
    private const int DoorbellTimeoutMs = 500;

    private readonly uint _serial;
    private readonly HMAudioOutput _output;
    private readonly HMMicrophoneInput _microphone;
    private readonly object _speakerLock = new();

    private MemoryMappedFile? _speakerMap;
    private MemoryMappedFile? _hapticMap;
    private MemoryMappedFile? _micMap;
    private MemoryMappedViewAccessor? _speakerView;
    private MemoryMappedViewAccessor? _hapticView;
    private MemoryMappedViewAccessor? _micView;
    private EventWaitHandle? _speakerDoorbell;
    private EventWaitHandle? _hapticDoorbell;
    private EventWaitHandle? _micDoorbell;
    private EventWaitHandle? _cancel;
    private Thread? _micThread;

    private readonly short[] _speakerPending = new short[SpeakerFlushFrames * WireSpeakerChannels];
    private int _speakerPendingFrames;
    private ushort _speakerSeq;
    // Same cadence as the speaker: both lanes come off one OUT frame, so
    // they fill and flush in lockstep under the one lock.
    private readonly short[] _hapticPending = new short[SpeakerFlushFrames * WireHapticChannels];
    private int _hapticPendingFrames;
    private ushort _hapticSeq;

    private readonly short[] _micSlot = new short[SlotSampleCapacity];
    private byte[] _micBytes = new byte[SlotSampleCapacity * 2];
    private uint _micLastSeq;

    private ulong _speakerHandle;
    private ulong _speakerEventHandle;
    private ulong _hapticHandle;
    private ulong _hapticEventHandle;
    private ulong _micHandle;
    private ulong _micEventHandle;

    private volatile bool _disposed;

    private AudioBridge(uint serial, HMUsbAudio audio)
    {
        _serial = serial;
        _output = audio.Output;
        _microphone = audio.Microphone;
    }

    internal static AudioBridge? TryCreate(uint serial, HMUsbAudio audio,
                                           Func<nint, ulong> duplicate)
    {
        var bridge = new AudioBridge(serial, audio);
        try
        {
            bridge.Start(duplicate);
            return bridge;
        }
        catch
        {
            bridge.Dispose();
            return null; // no endpoints on this pad; the pad itself is fine
        }
    }

    private void Start(Func<nint, ulong> duplicate)
    {
        _speakerMap = MemoryMappedFile.CreateNew(null, AudioSectionSize);
        _micMap = MemoryMappedFile.CreateNew(null, AudioSectionSize);
        _speakerView = _speakerMap.CreateViewAccessor();
        _micView = _micMap.CreateViewAccessor();
        _speakerDoorbell = new EventWaitHandle(false, EventResetMode.AutoReset);
        _micDoorbell = new EventWaitHandle(false, EventResetMode.AutoReset);
        _cancel = new EventWaitHandle(false, EventResetMode.ManualReset);

        _speakerHandle = duplicate(_speakerMap.SafeMemoryMappedFileHandle.DangerousGetHandle());
        _micHandle = duplicate(_micMap.SafeMemoryMappedFileHandle.DangerousGetHandle());
        _speakerEventHandle = duplicate(_speakerDoorbell.SafeWaitHandle.DangerousGetHandle());
        _micEventHandle = duplicate(_micDoorbell.SafeWaitHandle.DangerousGetHandle());
        if (_speakerHandle == 0 || _micHandle == 0 || _speakerEventHandle == 0 ||
            _micEventHandle == 0)
            throw new InvalidOperationException("could not duplicate the audio ring handles");

        // The haptic ring exists only where the endpoint has the lanes.
        // Its failure is its own: the speaker and mic rings above are
        // already handed over, and a pad without haptics beats a pad
        // without audio.
        if (_output.Channels >= HapticSourceChannels)
        {
            try
            {
                _hapticMap = MemoryMappedFile.CreateNew(null, AudioSectionSize);
                _hapticView = _hapticMap.CreateViewAccessor();
                _hapticDoorbell = new EventWaitHandle(false, EventResetMode.AutoReset);
                _hapticHandle = duplicate(_hapticMap.SafeMemoryMappedFileHandle.DangerousGetHandle());
                _hapticEventHandle = duplicate(_hapticDoorbell.SafeWaitHandle.DangerousGetHandle());
                if (_hapticHandle == 0 || _hapticEventHandle == 0) DropHapticRing();
            }
            catch
            {
                DropHapticRing();
            }
        }

        _output.FramesReceived += OnOutputFrames;

        // Background so a wedged drain can never hold the process open, and
        // so the plug/unplug control path never waits on audio work.
        _micThread = new Thread(MicLoop) { IsBackground = true, Name = "hm-mic-drain" };
        _micThread.Start();
    }

    internal void WriteHandshake(Utf8JsonWriter w)
    {
        w.WriteNumber("speakerAudio", _speakerHandle);
        w.WriteNumber("speakerAudioEvent", _speakerEventHandle);
        w.WriteNumber("micAudio", _micHandle);
        w.WriteNumber("micAudioEvent", _micEventHandle);
        // What the ring carries, so satellite can rate-convert. Channels
        // are the ENDPOINT's (satellite spreads mono across them on the
        // way in); the rate is the endpoint's on both rings.
        w.WriteNumber("speakerChannels", WireSpeakerChannels);
        w.WriteNumber("speakerRateHz", _output.SampleRateHz);
        w.WriteNumber("micChannels", _microphone.Channels);
        w.WriteNumber("micRateHz", _microphone.SampleRateHz);
        if (_hapticHandle != 0)
        {
            w.WriteNumber("hapticAudio", _hapticHandle);
            w.WriteNumber("hapticAudioEvent", _hapticEventHandle);
            w.WriteNumber("hapticChannels", WireHapticChannels);
        }
    }

    // Caller holds no lock: only reached from Start, before the frame
    // handler is subscribed.
    private void DropHapticRing()
    {
        _hapticView?.Dispose();
        _hapticView = null;
        _hapticMap?.Dispose();
        _hapticMap = null;
        _hapticDoorbell?.Dispose();
        _hapticDoorbell = null;
        _hapticHandle = 0;
        _hapticEventHandle = 0;
    }

    // The game's audio, straight off the emulated pad's OUT endpoint. On a
    // DualSense that is four channels: 1/2 are the speaker/headset lanes,
    // 3/4 the HD-haptics lanes, and each pair goes to its own ring so
    // satellite can gate and encode them apart. Runs on the SDK's own
    // audio pump thread.
    private void OnOutputFrames(HMAudioOutput source, ReadOnlyMemory<byte> pcm)
    {
        if (_disposed) return;
        int channels = source.Channels;
        if (channels < WireSpeakerChannels || source.BitsPerSample != 16) return;

        ReadOnlySpan<byte> bytes = pcm.Span;
        int stride = channels * 2;
        int frames = bytes.Length / stride;
        lock (_speakerLock)
        {
            if (_disposed) return;
            bool haptics = _hapticView != null && channels >= HapticSourceChannels;
            for (int f = 0; f < frames; f++)
            {
                int b = f * stride;
                int o = _speakerPendingFrames * WireSpeakerChannels;
                _speakerPending[o] = unchecked((short)(bytes[b] | (bytes[b + 1] << 8)));
                _speakerPending[o + 1] = unchecked((short)(bytes[b + 2] | (bytes[b + 3] << 8)));
                _speakerPendingFrames++;
                if (haptics)
                {
                    int h = _hapticPendingFrames * WireHapticChannels;
                    _hapticPending[h] = unchecked((short)(bytes[b + 4] | (bytes[b + 5] << 8)));
                    _hapticPending[h + 1] = unchecked((short)(bytes[b + 6] | (bytes[b + 7] << 8)));
                    _hapticPendingFrames++;
                }
                if (_speakerPendingFrames >= SpeakerFlushFrames)
                {
                    FlushSpeakerLocked();
                    FlushHapticLocked();
                }
            }
        }
    }

    private void FlushSpeakerLocked()
    {
        if (_speakerPendingFrames <= 0 || _speakerView == null) return;
        WriteSlot(_speakerView, _serial, _speakerSeq, _speakerPending,
                  _speakerPendingFrames * WireSpeakerChannels);
        _speakerSeq++;
        _speakerPendingFrames = 0;
        _speakerDoorbell?.Set();
    }

    private void FlushHapticLocked()
    {
        if (_hapticPendingFrames <= 0 || _hapticView == null) return;
        WriteSlot(_hapticView, _serial, _hapticSeq, _hapticPending,
                  _hapticPendingFrames * WireHapticChannels);
        _hapticSeq++;
        _hapticPendingFrames = 0;
        _hapticDoorbell?.Set();
    }

    private void MicLoop()
    {
        WaitHandle[] waits = { _cancel!, _micDoorbell! };
        while (!_disposed)
        {
            int rc = WaitHandle.WaitAny(waits, DoorbellTimeoutMs);
            if (rc == 0) return; // cancelled
            if (_disposed) return;

            MemoryMappedViewAccessor? view = _micView;
            if (view == null) return;
            while (ReadSlot(view, ref _micLastSeq, _micSlot, out uint slotSerial,
                            out int samples))
            {
                if (slotSerial != _serial || samples <= 0) continue;
                SubmitMic(samples);
            }
        }
    }

    // Mono at the endpoint's rate in, whatever the endpoint's channel count
    // wants out. Submitted unconditionally rather than gated on
    // IsStreaming: the SDK's own ring absorbs and drops what nobody is
    // recording, and a silent mic that depends on an alt-setting event
    // having fired is far harder to diagnose than a few wasted memcpys.
    private void SubmitMic(int monoSamples)
    {
        int channels = _microphone.Channels;
        if (channels < 1) channels = 1;
        int needed = monoSamples * channels * 2;
        if (_micBytes.Length < needed) _micBytes = new byte[needed];

        int o = 0;
        for (int i = 0; i < monoSamples; i++)
        {
            byte lo = (byte)(_micSlot[i] & 0xFF);
            byte hi = (byte)((_micSlot[i] >> 8) & 0xFF);
            // The DualSense mic function is declared stereo; the same
            // sample goes to every channel rather than silence on one.
            for (int c = 0; c < channels; c++)
            {
                _micBytes[o++] = lo;
                _micBytes[o++] = hi;
            }
        }
        _microphone.Submit(new ReadOnlySpan<byte>(_micBytes, 0, needed));
    }

    // Ring producer. Single writer per ring, so reserving the sequence is a
    // plain increment. Zeroing the slot's SeqNo first marks it in progress:
    // no published batch carries sequence 0, so a reader mid-write sees a
    // value matching no expectation and retries.
    private static void WriteSlot(MemoryMappedViewAccessor view, uint serial, ushort seq,
                                  short[] pcm, int sampleCount)
    {
        if (sampleCount > SlotSampleCapacity) return;
        uint next = view.ReadUInt32(0) + 1;
        view.Write(0, next);

        long slot = RingHeaderSize + (long)((next - 1) % RingSlots) * SlotSize;
        view.Write(slot + SlotSeqNoOffset, (uint)0);
        Thread.MemoryBarrier();

        view.Write(slot + SlotSerialOffset, serial);
        view.Write(slot + SlotSeqOffset, seq);
        view.Write(slot + SlotSamplesOffset, (ushort)sampleCount);
        if (sampleCount > 0) view.WriteArray(slot + SlotDataOffset, pcm, 0, sampleCount);

        Thread.MemoryBarrier();
        view.Write(slot + SlotSeqNoOffset, next);
    }

    // Ring consumer, the mirror of readNextAudioSlot: lap-skip to the
    // oldest still-readable slot, then a seqlock retry around the copy.
    private static bool ReadSlot(MemoryMappedViewAccessor view, ref uint lastSeq, short[] dst,
                                 out uint serial, out int sampleCount)
    {
        serial = 0;
        sampleCount = 0;
        uint head = view.ReadUInt32(0);
        if (head == lastSeq) return false;

        uint nextSeq = lastSeq + 1;
        if (head > nextSeq + RingSlots - 1) nextSeq = head - RingSlots + 1;
        long slot = RingHeaderSize + (long)((nextSeq - 1) % RingSlots) * SlotSize;

        for (int retries = 0; retries < 4; retries++)
        {
            uint before = view.ReadUInt32(slot + SlotSeqNoOffset);
            if (before != nextSeq) return false;
            Thread.MemoryBarrier();

            uint slotSerial = view.ReadUInt32(slot + SlotSerialOffset);
            int samples = view.ReadUInt16(slot + SlotSamplesOffset);
            if (samples > SlotSampleCapacity) samples = SlotSampleCapacity;
            if (samples > 0) view.ReadArray(slot + SlotDataOffset, dst, 0, samples);

            Thread.MemoryBarrier();
            if (view.ReadUInt32(slot + SlotSeqNoOffset) == before)
            {
                lastSeq = nextSeq;
                serial = slotSerial;
                sampleCount = samples;
                return true;
            }
        }
        return false;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        try { _output.FramesReceived -= OnOutputFrames; }
        catch { /* the controller may already be torn down */ }

        // _disposed is already set, so taking the lock waits out an
        // in-flight frame handler and guarantees no later one runs: the
        // view must not be unmapped underneath a memcpy.
        lock (_speakerLock)
        {
            _speakerView?.Dispose();
            _speakerView = null;
            _speakerMap?.Dispose();
            _speakerMap = null;
            _speakerDoorbell?.Dispose();
            _speakerDoorbell = null;
            _hapticView?.Dispose();
            _hapticView = null;
            _hapticMap?.Dispose();
            _hapticMap = null;
            _hapticDoorbell?.Dispose();
            _hapticDoorbell = null;
        }

        _cancel?.Set();
        bool joined = true;
        if (_micThread != null)
        {
            try { joined = _micThread.Join(2000); }
            catch { joined = false; }
            _micThread = null;
        }

        // A drain thread that would not join still holds the mapped view.
        // Unmapping it would be an access violation, so a wedged bridge
        // leaks its 128 KB section instead: bounded, and the process exits
        // with satellite anyway.
        if (joined)
        {
            _micView?.Dispose();
            _micView = null;
            _micMap?.Dispose();
            _micMap = null;
            _micDoorbell?.Dispose();
            _micDoorbell = null;
            _cancel?.Dispose();
            _cancel = null;
        }
    }
}
