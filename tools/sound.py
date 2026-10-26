"""List and decode the sound of Rampage: Total Destruction (PS2): the .uber/.samp sound banks and the .mic music.

See docs/FORMAT_SOUND.md. The files are read from build/vol (tools/pvol.py extracts RAMPAGE.VOL there). The Wii's
banks (its Audio folder) are the same, big endian ("REBU"), with DSP-ADPCM samples; its music is Bink video files.

Usage:
  python tools/sound.py events [BANK]           every event (sound name) with its PROJ entry and program
  python tools/sound.py samples [BANK]          the sample directory (SDIR) with decoded lengths and peaks
  python tools/sound.py wav BANK INDEX OUT.wav  one sample (SDIR index) as a 16-bit WAV
  python tools/sound.py mic NAME [OUT.wav]      a music stream: header, length, peak (and a WAV if asked)
  python tools/sound.py check                   decode everything, report lengths / peaks / bad frames
  python tools/sound.py --verify DIR            check every bank in DIR (build/vol, or the Wii's Audio folder)
BANK is Rampage (default) or RShell, or the path of a .uber.
"""
import os
import struct
import sys
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VOL = os.path.join(ROOT, 'build', 'vol')

# PS-ADPCM prediction filters (SPU), in 1/64
FILTERS = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def adpcm_decode(data, hist=(0, 0), stop_at_end=True):
    """Decode PS-ADPCM frames (16 bytes: shift|filter<<4, flags, 14 bytes of nibbles) to a list of int16.
    Returns (samples, flags of each frame, hist)."""
    out = []
    flags = []
    h1, h2 = hist
    for o in range(0, len(data) - 15, 16):
        sf, fl = data[o], data[o + 1]
        shift, filt = sf & 15, (sf >> 4) & 15
        if filt > 4:
            filt = 0
        if shift > 12:
            shift = 9
        f0, f1 = FILTERS[filt]
        for b in data[o + 2:o + 16]:
            for n in (b & 15, b >> 4):
                s = (n - 16 if n & 8 else n) << 12
                s >>= shift
                s += (h1 * f0 + h2 * f1 + 32) >> 6
                s = max(-32768, min(32767, s))
                out.append(s)
                h2, h1 = h1, s
        flags.append(fl)
        if stop_at_end and fl & 1:
            break
    return out, flags, (h1, h2)


def dsp_decode(data, start, count, coefs, hist=(0, 0)):
    """Decode `count` nibbles of DSP-ADPCM from nibble address `start` in `data` (frames of 8 bytes: a header,
    predictor << 4 | scale, then 14 nibbles; the header's two nibbles count in the addresses). Returns (samples,
    the header of every frame)."""
    out, headers = [], []
    h1, h2 = hist
    for a in range(start, start + count):
        if a % 16 < 2:                               # the frame header's nibbles
            continue
        ps = data[a // 16 * 8]
        if a % 16 == 2 or not headers:
            headers.append(ps)
        c1, c2 = coefs[2 * (ps >> 4)], coefs[2 * (ps >> 4) + 1]
        n = data[a // 2] >> 4 if a % 2 == 0 else data[a // 2] & 15
        n = n - 16 if n & 8 else n
        s = ((n << (ps & 15)) << 11) + 1024 + c1 * h1 + c2 * h2 >> 11
        s = max(-32768, min(32767, s))
        out.append(s)
        h2, h1 = h1, s
    return out, headers


class Bank:
    """A .uber sound bank: PROJ (events), POOL (their programs), SDIR (sample directory), groups, names.
    The Wii's is big endian ("REBU"), its SDIR entries 64 bytes: u32 nibble address, u32 nibbles, u32 flags, u32
    rate, s16[16] DSP-ADPCM coefficients, u16 gain, u16 the first frame's header, then zeros."""

    def __init__(self, name='Rampage'):
        base = name[:-5] if name.endswith('.uber') else os.path.join(VOL, name)
        with open(base + '.uber', 'rb') as f:
            d = f.read()
        self.samp_path = base + '.samp'
        if d[:4] not in (b'UBER', b'REBU'):
            raise ValueError(f'{base}.uber is not a sound bank')
        self.wii = d[:4] == b'REBU'
        e = self.e = '>' if self.wii else '<'
        proj, pool, sdir, gcount, gnames, names = struct.unpack_from(e + '6I', d, 8)
        n = struct.unpack_from(e + 'H', d, names)[0]           # u16 counts, then 2 bytes
        self.names = [d[o:d.index(b'\0', o)].decode() for o in struct.unpack_from(f'{e}{n}I', d, names + 4)]
        ng = struct.unpack_from(e + 'H', d, gnames)[0]
        self.groups = [d[o:d.index(b'\0', o)].decode() for o in struct.unpack_from(f'{e}{ng}I', d, gnames + 4)]
        self.group_counts = struct.unpack_from(f'{e}{ng}H', d, gcount)
        pn = struct.unpack_from(e + 'I', d, proj + 8)[0]
        self.proj = [struct.unpack_from(e + 'fIHBB', d, proj + 16 + 12 * i) for i in range(pn)]
        qn = struct.unpack_from(e + 'I', d, pool + 8)[0]
        offs = list(struct.unpack_from(f'{e}{qn}I', d, pool + 16))
        self.progs = []
        for i, o in enumerate(offs):
            end = min([x for x in offs if x > o] + [sdir - pool])
            self.progs.append(d[pool + o:pool + end])
        sn = struct.unpack_from(e + 'I', d, sdir + 12)[0]
        self.samp_size = struct.unpack_from(e + 'I', d, sdir + 8)[0]
        stride = 64 if self.wii else 16
        self.sdir = [struct.unpack_from(e + '4I', d, sdir + 16 + stride * i) for i in range(sn)]
        self.dsp = None
        if self.wii:                                 # (coefficients, (gain, first frame header)) per sample
            self.dsp = [(struct.unpack_from('>16h', d, sdir + 32 + 64 * i),
                         struct.unpack_from('>HH', d, sdir + 64 + 64 * i)) for i in range(sn)]

    def sample(self, i):
        off, size, flags, rate = self.sdir[i]
        if self.wii:                                 # the whole frames holding the nibbles
            off, size = off // 16 * 8, (off + size + 15) // 16 * 8 - off // 16 * 8
        with open(self.samp_path, 'rb') as f:
            f.seek(off)
            return f.read(size), flags, rate

    def decode(self, i):
        """(samples, the frames' flags (PS2) or headers (Wii)) of sample i."""
        data, _, _ = self.sample(i)
        if not self.wii:
            pcm, fl, _ = adpcm_decode(data)
            return pcm, fl
        return dsp_decode(data, self.sdir[i][0] % 16, self.sdir[i][1], self.dsp[i][0])


def disasm(ins):
    op = ins[0]
    be = lambda a: ins[a] << 8 | ins[a + 1]
    if op == 0x00:
        return 'end'
    if op == 0x05:
        return f'loop to {be(4)} x{be(6)}' + (' (until stopped)' if ins[1] else '')
    if op == 0x06:
        return f'goto event {be(2)} @{be(4)}'
    if op == 0x07:
        what = []
        if ins[1]:
            what.append('stop')
        if ins[3]:
            what.append('voice end')
        if ins[4]:
            what.append(f'{be(6)} ms')
        return 'wait ' + (' or '.join(what) if what else 'forever')
    if op == 0x08:
        return f'start event {be(2)} @{be(4)}'
    if op == 0x0D:
        return f'volume x{ins[1]}/128'
    if op == 0x0F:
        return f'fade to {ins[1]} in {be(6)} ms'
    if op == 0x10:
        return f'play sample {be(1)}'
    if op == 0x11:
        return 'stop voice'
    if op == 0x13:
        return f'if rand>{ins[1]} goto event {be(2)} @{be(4)}'
    if op == 0x14:
        return 'next of @' + ','.join(str(b) for b in ins[1:7] if b)
    if op == 0x32:
        return f'flag[{ins[1]}] = {ins[2]}'
    return 'op %02x ' % op + ins[1:].hex(' ')


def wav_write(path, pcm, rate, channels=1):
    with wave.open(path, 'wb') as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f'<{len(pcm)}h', *pcm))


def mic_read(name):
    with open(os.path.join(VOL, name + '.mic'), 'rb') as f:
        d = f.read()
    head, unk, ch, rate, inter, blocks = struct.unpack_from('<6I', d, 0)
    return d, head, unk, ch, rate, inter, blocks


def mic_decode(name):
    d, head, unk, ch, rate, inter, blocks = mic_read(name)
    chans = [[] for _ in range(ch)]
    hist = [(0, 0)] * ch
    o = head
    for b in range(blocks):
        for c in range(ch):
            pcm, fl, hist[c] = adpcm_decode(d[o:o + inter], hist[c], stop_at_end=False)
            chans[c] += pcm
            o += inter
    return chans, rate


def verify(folder):
    """Every bank in `folder`: its tables, and every sample decoded. PS2: each ends on an end frame, or a looping one
    (flags bit 0) starts with the loop start. Wii: the samples
    follow each other through the .samp, each starts with the frame header its entry gives and uses only the
    predictors 0..7."""
    bad = 0
    for f in sorted(os.listdir(folder)):
        if not f.lower().endswith('.uber'):
            continue
        b = Bank(os.path.join(folder, f))
        size = os.path.getsize(b.samp_path)
        problems = []
        if len(b.proj) != len(b.names) or len(b.progs) != len(b.proj):
            problems.append(f'{len(b.proj)} events, {len(b.names)} names, {len(b.progs)} programs')
        if b.samp_size != size:
            problems.append(f'SDIR gives the .samp {b.samp_size} bytes, it has {size}')
        samples, seconds, end = 0, 0.0, 0
        for i, (off, n, flags, rate) in enumerate(b.sdir):
            if off == 0xFFFFFFFF:
                continue
            pcm, fl = b.decode(i)
            samples += 1
            seconds += len(pcm) / rate
            if b.wii:
                if off // 16 * 8 != end or off % 16 != 2:
                    problems.append(f'sample {i} at nibble {off:#x}, the one before ends at byte {end:#x}')
                end = (off + n + 15) // 16 * 8
                if fl[0] != b.dsp[i][1][1] or any(h >> 4 > 7 for h in fl):
                    problems.append(f'sample {i}: frame headers {fl[:4]}, its entry gives {b.dsp[i][1][1]:#x}')
            elif not pcm or not (fl[0] & 4 if flags & 1 else fl[-1] & 1):
                problems.append(f'sample {i} has no ' + ('loop start' if flags & 1 else 'end frame'))
        if b.wii and end > size:
            problems.append(f'samples end at {end:#x}, the .samp is {size:#x}')
        for p in problems[:5]:
            print(f'{f}: {p}')
        bad += len(problems)
        print(f'{f}: {len(b.proj)} events, {samples} samples ({seconds:.1f} s, '
              f'{"DSP-ADPCM" if b.wii else "PS-ADPCM"}); {len(problems)} problems')
    return not bad


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    if a[0] == 'events':
        b = Bank(a[1] if len(a) > 1 else 'Rampage')
        print('groups:', list(zip(b.groups, b.group_counts)))
        for i, (vol, flags, ev, grp, maxn) in enumerate(b.proj):
            print(f'{i:4} {b.names[i]:32} range x{vol:<6g} flags {flags:#04x} group {grp} max {maxn}')
            p = b.progs[ev]
            for k in range(0, len(p), 8):
                print(f'        @{k // 8:<3} {p[k:k + 8].hex(" ")}   {disasm(p[k:k + 8])}')
    elif a[0] == 'samples':
        b = Bank(a[1] if len(a) > 1 else 'Rampage')
        for i, (off, size, flags, rate) in enumerate(b.sdir):
            if off == 0xFFFFFFFF:
                print(f'{i:4} (none)')
                continue
            pcm, fl = b.decode(i)
            peak = max((abs(x) for x in pcm), default=0)
            print(f'{i:4} off {off:#9x} size {size:6} flags {flags} {rate:5} Hz  {len(pcm) / rate:6.3f} s  peak {peak:5}'
                  f'  frame flags {sorted(set(fl))}')
    elif a[0] == 'wav':
        b = Bank(a[1])
        wav_write(a[3], b.decode(int(a[2]))[0], b.sdir[int(a[2])][3])
    elif a[0] == 'mic':
        d, head, unk, ch, rate, inter, blocks = mic_read(a[1])
        print(f'header {head:#x} field4 {unk:#x} channels {ch} rate {rate} interleave {inter:#x} blocks {blocks}'
              f' file {len(d)} = {head + blocks * inter * ch}')
        chans, rate = mic_decode(a[1])
        print(f'{len(chans[0]) / rate:.2f} s, peak {max(abs(x) for c in chans for x in c)}')
        if len(a) > 2:
            wav_write(a[2], [s for fr in zip(*chans) for s in fr], rate, ch)
    elif a[0] == 'check':
        for name in ('Rampage', 'RShell'):
            b = Bank(name)
            bad = 0
            for i, (off, size, flags, rate) in enumerate(b.sdir):
                if off == 0xFFFFFFFF:
                    continue
                pcm, fl, _ = adpcm_decode(b.sample(i)[0])
                if not pcm or not fl[-1] & 1:
                    bad += 1
            print(f'{name}: {len(b.proj)} events, {len(b.sdir) - 1} samples, {bad} without an end frame')
    elif a[0] == '--verify' and len(a) == 2:
        sys.exit(0 if verify(a[1]) else 1)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
