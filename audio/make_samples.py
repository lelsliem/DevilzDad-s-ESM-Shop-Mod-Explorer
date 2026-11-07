import os
import numpy as np
import wave

SR = 44100
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "samples")
os.makedirs(OUT, exist_ok=True)

rng = np.random.default_rng(0xD3E1)


def write_wav(name, data, target=0.85):
    data = np.clip(np.asarray(data, dtype=np.float64), -1.0, 1.0)
    peak = float(np.max(np.abs(data))) or 1.0
    data = data * (target / peak)
    if data.ndim == 1:
        data = np.stack([data, data], axis=1)
    pcm = (data * 32767.0).astype("<i2")
    path = os.path.join(OUT, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print(f"{name}: {len(pcm)} frames, {os.path.getsize(path)} bytes")


def one_pole_lp(x, cutoff):
    alpha = 1.0 - np.exp(-2.0 * np.pi * cutoff / SR)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc += alpha * (x[i] - acc)
        y[i] = acc
    return y


def hp(x, cutoff):
    return x - one_pole_lp(x, cutoff)


def band(x, lo, hi):
    return one_pole_lp(x, hi) - one_pole_lp(x, lo)


def noise(n):
    return rng.standard_normal(n)


def perc(n, decay):
    return np.exp(-np.arange(n) / SR / max(1e-6, decay))


def ar(n, atk=0.005, rel=0.08):
    a = min(n, max(1, int(atk * SR)))
    r = min(n - a, max(1, int(rel * SR)))
    e = np.ones(n)
    e[:a] = np.linspace(0.0, 1.0, a) ** 1.4
    e[n - r:] = np.linspace(1.0, 0.0, r) ** 0.8
    return e


def add_wrap(buf, start, sig):
    n = len(buf)
    idx = (start + np.arange(len(sig))) % n
    np.add.at(buf, idx, sig)


def soften(x, cutoff=4200.0, atk=0.010, rel=0.020):
    """Take the edge off a UI one-shot.

    The old stings were written at 0.7-0.95 peak with a 4 ms attack and a
    bright noise tick, which reads as a crack over a game already mixing
    dialogue and weapons. This trims the top end, ramps the onset in instead
    of starting on a step (the step is what clicks), and lets the tail fall
    to zero so the clip never ends on a discontinuity.
    """
    y = one_pole_lp(np.asarray(x, dtype=np.float64), cutoff)
    n = len(y)
    a = min(n // 2, max(1, int(atk * SR)))
    r = min(n - a, max(1, int(rel * SR)))
    y[:a] *= np.linspace(0.0, 1.0, a) ** 1.2
    y[n - r:] *= np.linspace(1.0, 0.0, r) ** 1.1
    return y


def reverb(x, decay=1.9, mix=0.30, predelay=0.018, tone=5200.0):
    """Convolve with a synthetic room tail, wrapping the ring-out.

    The ring-out is folded back onto the head of the loop rather than
    truncated, so the seam at the loop point keeps its tail instead of
    falling silent for a beat.
    """
    n = int(decay * SR)
    t = np.arange(n) / SR
    ir = rng.standard_normal(n) * np.exp(-t / (decay * 0.30))
    ir = one_pole_lp(ir, tone)
    ir /= np.sqrt(float(np.sum(ir * ir))) or 1.0
    ir = np.concatenate([np.zeros(int(predelay * SR)), ir])
    size = 1 << (len(x) + len(ir) - 2).bit_length()
    full = np.fft.irfft(np.fft.rfft(x, size) * np.fft.rfft(ir, size), size)[: len(x) + len(ir) - 1]
    wet = full[: len(x)].copy()
    ring = full[len(x):]
    if len(ring):
        wet[: len(ring)] += ring[: len(wet)]
    return (1.0 - mix) * x + mix * wet


def soft_clip(x, drive=1.25):
    """tanh on a normalised signal: keeps transients below a hard corner."""
    peak = float(np.max(np.abs(x))) or 1.0
    return np.tanh(x / peak * drive) / np.tanh(drive)





def click(dur=0.055, tone=2600.0, body=520.0, amp=0.85, noise_amt=0.5):
    n = int(dur * SR)
    t = np.arange(n) / SR
    tick = hp(noise(n), 1800.0) * np.exp(-t / 0.004) * noise_amt
    ring = np.sin(2 * np.pi * tone * t) * np.exp(-t / 0.010)
    thump = np.sin(2 * np.pi * body * t) * np.exp(-t / 0.020) * 0.6
    sig = (tick + ring * 0.5 + thump) * amp
    pad = np.zeros(int(0.02 * SR))
    return np.concatenate([pad, sig])


def make_click():
    # Softened 2026-10-18: -3.7 dB peak, lower body, a third of the noise
    # tick and a real fade-in instead of a step. A menu click should sit
    # under dialogue, not over it.
    write_wav("ui_click.wav",
              soften(click(amp=0.62, noise_amt=0.22, tone=2150.0, body=455.0), 3800.0, 0.008, 0.030),
              target=0.62)


def make_hover():
    write_wav("ui_hover.wav",
              soften(click(dur=0.030, tone=2600.0, body=760.0, amp=0.30, noise_amt=0.14), 3200.0, 0.006, 0.018),
              target=0.40)


def make_select():
    dur = 0.26
    n = int(dur * SR)
    t = np.arange(n) / SR
    a = np.sin(2 * np.pi * 660.0 * t) * np.exp(-t / 0.07)
    shift = int(0.055 * SR)
    b = np.zeros(n)
    tb = t[: n - shift]
    b[shift:] = np.sin(2 * np.pi * 990.0 * tb) * np.exp(-tb / 0.09)
    sh = hp(noise(n), 1200.0) * np.exp(-t / 0.003) * 0.09
    write_wav("ui_select.wav", soften((a * 0.5 + b * 0.55 + sh) * 0.8, 4000.0, 0.010, 0.060), target=0.50)


def make_open():
    dur = 1.35
    n = int(dur * SR)
    t = np.arange(n) / SR
    x = t / dur
    swell = np.sin(np.pi * np.clip(x, 0.0, 1.0)) ** 1.35
    slow = np.sin(2 * np.pi * (47.0 + 9.0 * np.sin(2 * np.pi * 0.45 * t)) * t + 0.5 * np.sin(2 * np.pi * 0.3 * t))
    rumble = slow * (0.5 * swell)
    nz = noise(n)
    growl = (one_pole_lp(nz, 1000.0) - one_pole_lp(nz, 240.0)) * swell ** 1.25
    breath = hp(nz, 2400.0) * np.exp(-(((x - 0.72) / 0.32) ** 2)) * 0.18
    sig = growl * 1.05 + rumble * 0.6 + breath
    sig = one_pole_lp(sig * ar(n, 0.06, 0.40), 4400.0)
    write_wav("ui_open.wav", soften(sig, 4400.0, 0.020, 0.080), target=0.55)


def make_close():
    dur = 0.55
    n = int(dur * SR)
    t = np.arange(n) / SR
    sweep = np.sin(2 * np.pi * (700.0 * t - 560.0 * t * t)) * np.exp(-t / 0.20)
    air = hp(noise(n), 2600.0) * np.exp(-t / 0.14) * 0.12
    sub = np.sin(2 * np.pi * 128.0 * t) * np.exp(-t / 0.20) * 0.4
    write_wav("ui_close.wav", soften((sweep * 0.4 + air + sub) * 0.7, 4200.0, 0.012, 0.070), target=0.50)


def make_denied():
    dur = 0.22
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = 215.0
    # Square waves are the harshest thing in the set; low-pass them before
    # the envelope so the refusal reads as a dull thud, not a buzzer.
    sq = one_pole_lp(np.sign(np.sin(2 * np.pi * f * t)) * 0.5, 1200.0)
    sq2 = one_pole_lp(np.sign(np.sin(2 * np.pi * (f * 0.84) * t)) * 0.5, 1200.0)
    sh = hp(noise(n), 900.0) * np.exp(-t / 0.004) * 0.08
    write_wav("ui_denied.wav", soften(((sq + sq2) * perc(n, 0.12) + sh) * 0.55, 1800.0, 0.012, 0.040),
              target=0.45)


NOTE = {"D2": 73.42, "F2": 87.31, "G2": 98.00, "A2": 110.00, "Bb1": 58.27, "Bb2": 116.54, "C2": 65.41,
        "C3": 130.81, "D3": 146.83, "E3": 164.81, "F3": 174.61, "G3": 196.00, "A3": 220.00,
        "Bb3": 233.08, "C4": 261.63, "D4": 293.66, "E4": 329.63, "F4": 349.23, "G4": 392.00,
        "A4": 440.00, "Bb4": 466.16, "C5": 523.25, "D5": 587.33, "E5": 659.26, "F5": 698.46, "F2": 87.31,
        "A5": 880.00, "Bb5": 932.33}


def vibes(freq, dur, amp=0.30):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = freq * (1.0 + 0.0045 * np.sin(2 * np.pi * 5.2 * t))
    ph = 2 * np.pi * np.cumsum(f) / SR
    sig = np.sin(ph) + 0.30 * np.sin(2 * ph) + 0.13 * np.sin(3 * ph) + 0.06 * np.sin(4 * ph)
    sig = one_pole_lp(sig, 5200.0) * ar(n, 0.014, 0.10) * perc(n, dur * 0.5)
    return sig * amp


def rhodes(freq, dur, amp=0.30):
    n = int(dur * SR)
    t = np.arange(n) / SR
    idx = 2.7 * np.exp(-t / 0.18)
    sig = np.sin(2 * np.pi * freq * t + idx * np.sin(2 * np.pi * freq * 2.0 * t))
    sig = sig + 0.22 * np.sin(2 * np.pi * freq * 2.0 * t) * np.exp(-t / 0.09)
    sig = one_pole_lp(sig, 4200.0)
    sig = sig * (1.0 + 0.15 * np.sin(2 * np.pi * 5.4 * t))
    sig = sig * ar(n, 0.006, 0.12) * perc(n, dur * 0.55)
    return sig * amp


def ubass(freq, dur, amp=0.50):
    n = int(dur * SR)
    t = np.arange(n) / SR
    sig = np.zeros(n)
    for k, g in ((1, 1.0), (2, 0.52), (3, 0.33), (4, 0.19), (5, 0.11), (6, 0.07)):
        sig += np.sin(2 * np.pi * freq * k * t) * g
    sig = one_pole_lp(sig, 2300.0) * ar(n, 0.008, 0.10) * perc(n, dur * 0.45)
    return sig * amp


def horn(freq, dur, amp=0.20):
    n = int(dur * SR)
    t = np.arange(n) / SR
    sig = np.zeros(n)
    for k, g in ((1, 1.0), (2, 0.62), (3, 0.46), (4, 0.30), (5, 0.20), (6, 0.13), (7, 0.08)):
        sig += np.sin(2 * np.pi * freq * k * t) * g
    sig = band(sig, 520.0, 2500.0) * ar(n, 0.022, 0.10) * perc(n, dur * 0.5)
    return sig * amp


def brush(dur, amp=0.10):
    n = int(dur * SR)
    t = np.arange(n) / SR
    swirl = np.sin(np.pi * np.clip(t / (dur * 0.55), 0.0, 1.0)) ** 1.5
    swirl = np.maximum(swirl, 0.35)
    trem = 0.62 + 0.38 * np.sin(2 * np.pi * 7.3 * t) ** 2
    return band(noise(n), 2200.0, 7000.0) * swirl * trem * amp


def tick(dur=0.055, amp=0.055, f=5400.0):
    n = int(dur * SR)
    t = np.arange(n) / SR
    sig = hp(noise(n), f * 0.75) * 0.8 + 0.35 * np.sin(2 * np.pi * f * t)
    return sig * perc(n, 0.016) * amp


def kick(dur=0.30, amp=0.42):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = 95.0 * np.exp(-t / 0.045) + 50.0
    ph = 2 * np.pi * np.cumsum(f) / SR
    return np.sin(ph) * perc(n, 0.10) * ar(n, 0.004, 0.05) * amp


def rimshot(dur=0.10, amp=0.16):
    n = int(dur * SR)
    t = np.arange(n) / SR
    sig = 0.55 * np.sin(2 * np.pi * 1780.0 * t) + 0.35 * np.sin(2 * np.pi * 2450.0 * t) + hp(noise(n), 1400.0) * 0.5
    return sig * perc(n, 0.024) * amp


def make_elevator():
    """Front-of-shop lounge bed: 76 bpm, 8 bars, no kick.

    The old loop leaned on a kick/rimshot backbeat and a busy vibes line,
    which is what made it fight the game mix. This keeps the lounge feel but
    puts the pulse in the bass and a soft shaker, opens the chord voicings
    (maj9 / m9 / 9) and runs everything through a short room, so it sits
    under dialogue instead of on top of it.
    """
    bpm = 76.0
    beat = 60.0 / bpm
    bar = beat * 4.0
    bars = 8
    n = int(round(bar * bars * SR))
    L = np.zeros(n)
    R = np.zeros(n)

    def put(sig, start_sec, pan=0.0, gain=1.0):
        s = int(round(start_sec * SR))
        add_wrap(L, s, sig * gain * (1.0 - max(0.0, pan)))
        add_wrap(R, s, sig * gain * (1.0 + min(0.0, pan)))

    chords = [
        (["F3", "A3", "C4", "G4"], "F2"),      # Fmaj9
        (["D3", "F3", "A3", "E4"], "D2"),      # Dm9
        (["Bb2", "D3", "F3", "A3"], "Bb1"),    # Bbmaj7
        (["C3", "E3", "G3", "Bb3"], "C2"),     # C9
    ]
    melody = ["A4", "F4", "D5", "C5", "A4", "F4", "G4", "E4"]

    for b in range(bars):
        t0 = b * bar
        notes, root = chords[b % 4]
        for i, nt in enumerate(notes):
            put(rhodes(NOTE[nt], bar * 1.05, amp=0.115), t0 + 0.003 * i,
                pan=(i - 1.5) * 0.18, gain=0.85)
            put(pad(NOTE[nt] * 2.0, bar * 1.05, amp=0.045), t0,
                pan=(1.5 - i) * 0.14, gain=0.7)
        put(rhodes(NOTE[notes[0]], beat * 1.4, amp=0.10), t0 + beat * 2.0, pan=-0.12, gain=0.75)
        for i in range(2):
            put(ubass(NOTE[root], beat * 1.7, amp=0.34), t0 + i * 2 * beat, gain=0.85)
        for step in range(8):
            put(brush(0.09, amp=0.030), t0 + step * (beat / 2.0),
                pan=0.16 if step % 2 else -0.16, gain=0.8)
        if b % 2 == 0:
            put(vibes(NOTE[melody[b // 2]], beat * 1.9, amp=0.115), t0 + beat, pan=0.05, gain=0.85)
        if b % 4 == 3:
            put(vibes(NOTE[melody[4 + b // 4]], beat * 1.2, amp=0.09), t0 + beat * 3.0, pan=-0.08, gain=0.8)

    hum = 0.0022 * np.sin(2 * np.pi * 60.0 * np.arange(n) / SR)
    L = reverb(L + hum + band(noise(n), 2200.0, 6000.0) * 0.0035, decay=1.7, mix=0.26, tone=4200.0)
    R = reverb(R + hum + band(noise(n), 2200.0, 6000.0) * 0.0035, decay=1.7, mix=0.26, tone=4200.0)
    st = np.stack([L, R], axis=1)
    # target=0.55, not 1.0: write_wav normalises the peak, so the old track
    # shipped at full scale and the runtime volume was doing all the work.
    # The bed now lands around -12 dBFS RMS before the volume slider.
    st = soft_clip(st / (float(np.max(np.abs(st))) or 1.0) * 0.85, drive=1.1)
    write_wav("shop_music_elevator.wav", st, target=0.55)
    print(f"elevator loop {bar * bars:.2f}s @ {bpm:.0f} bpm, {bars} bars, lounge bed")


def pluck(freq, dur, amp=0.5, detune=0.004, brightness=5200.0):
    n = int(dur * SR)
    t = np.arange(n) / SR
    sig = np.zeros(n)
    for k, g in ((1, 1.0), (2, 0.42), (3, 0.22), (4, 0.12), (5, 0.07)):
        sig += np.sin(2 * np.pi * freq * k * (1.0 + detune * k) * t) * g
    sig = one_pole_lp(sig, brightness) * np.exp(-t / (dur * 0.34))
    sig[: min(n, int(0.004 * SR))] *= np.linspace(0, 1, min(n, int(0.004 * SR)))
    return sig * amp


def pad(freq, dur, amp=0.18):
    n = int(dur * SR)
    t = np.arange(n) / SR
    sig = np.zeros(n)
    for k, g in ((1, 1.0), (2, 0.30), (3, 0.14), (4, 0.08)):
        sig += np.sin(2 * np.pi * freq * k * t) * g
    sig = one_pole_lp(sig, 2200.0)
    a = min(n, int(0.25 * SR))
    r = min(n - a, int(0.30 * SR))
    sig[:a] *= np.linspace(0, 1, a)
    sig[n - r:] *= np.linspace(1, 0, r)
    return sig * amp


def bass(freq, dur, amp=0.55):
    n = int(dur * SR)
    t = np.arange(n) / SR
    sig = np.sin(2 * np.pi * freq * t) + 0.30 * np.sin(2 * np.pi * freq * 2 * t) + 0.10 * np.sin(2 * np.pi * freq * 3 * t)
    sig *= np.exp(-t / (dur * 0.5))
    a = min(n, int(0.006 * SR))
    r = min(n - a, int(0.05 * SR))
    sig[:a] *= np.linspace(0, 1, a)
    sig[n - r:] *= np.linspace(1, 0, r)
    return sig * amp


def hat(dur=0.05, amp=0.12):
    n = int(dur * SR)
    t = np.arange(n) / SR
    sig = np.diff(one_pole_lp(noise(n), 9000.0), prepend=0.0) * np.exp(-t / 0.012)
    return sig * amp


def make_dark():
    """Shop ambience for the dark themes: 54 bpm, 8 bars, no percussion.

    Replaces a 100 bpm arpeggio loop that had a hat on every off-beat. Slow
    pad moves, a sub pulse and one bell per phrase - the shop is a menu, so
    the bed should be room tone with a harmony in it, not a track.
    """
    bpm = 54.0
    beat = 60.0 / bpm
    bar = beat * 4.0
    bars = 8
    n = int(round(bar * bars * SR))
    L = np.zeros(n)
    R = np.zeros(n)

    def put(sig, start_sec, pan=0.0, gain=1.0):
        s = int(round(start_sec * SR))
        add_wrap(L, s, sig * gain * (1.0 - max(0.0, pan)))
        add_wrap(R, s, sig * gain * (1.0 + min(0.0, pan)))

    chords = [
        (["D3", "F3", "A3", "E4"], "D2"),      # Dm9
        (["Bb2", "D3", "F3", "A3"], "Bb1"),    # Bbmaj7
        (["G2", "Bb2", "D3", "A3"], "G2"),     # Gm9
        (["A2", "D3", "E3", "G3"], "A2"),      # A7sus
    ]
    bell = ["D5", "F5", "A4", "C5", "D5", "Bb4", "G4", "A4"]

    for b in range(bars):
        t0 = b * bar
        notes, root = chords[b % 4]
        for i, nt in enumerate(notes):
            put(pad(NOTE[nt], bar * 1.10, amp=0.135), t0 + 0.01 * i,
                pan=(i - 1.5) * 0.22, gain=0.9)
        put(pad(NOTE[root], bar * 1.10, amp=0.08), t0, pan=0.0, gain=0.8)
        for i in range(2):
            put(bass(NOTE[root], beat * 2.4, amp=0.24), t0 + i * 2 * beat, gain=0.75)
        # One bell on the third beat, and a quieter answer on the turnaround bar.
        # pluck() is the soft half-tone generator already used by the themes;
        # a bright bell here reads as a notification chime over dialogue.
        put(pluck(NOTE[bell[b]], 2.6, amp=0.075, brightness=2400.0), t0 + beat * 2.0, pan=0.12, gain=0.8)
        if b % 4 == 3:
            put(pluck(NOTE[bell[4 + b // 4]], 2.2, amp=0.055, brightness=2400.0),
                t0 + beat * 3.5, pan=-0.14, gain=0.8)
        air_n = int(bar * 1.05 * SR)
        air = band(noise(air_n), 300.0, 3200.0) * np.sin(np.linspace(0.0, np.pi, air_n)) ** 2 * 0.010
        put(air, t0, gain=0.9)

    L = reverb(L, decay=2.6, mix=0.34, tone=3600.0)
    R = reverb(R, decay=2.6, mix=0.34, tone=3600.0)
    st = np.stack([L, R], axis=1)
    st = soft_clip(st / (float(np.max(np.abs(st))) or 1.0) * 0.85, drive=1.05)
    write_wav("shop_music_dark.wav", st, target=0.60)
    print(f"dark loop {bar * bars:.2f}s @ {bpm:.0f} bpm, {bars} bars, ambient bed")


def make_remove():
    """Taking a line out of the basket, a store back, a cleared list.

    Same voice as ui_click - the same tick, the same level - with the tone
    falling away instead of holding: two short notes a fifth apart, the second
    lower, so it reads as subtraction beside the rising ui_select. Deliberately
    shorter and quieter than ui_close, which is the whole shop closing.
    """
    dur = 0.17
    n = int(dur * SR)
    t = np.arange(n) / SR
    a = np.sin(2 * np.pi * 620.0 * t) * np.exp(-t / 0.028)
    shift = int(0.048 * SR)
    b = np.zeros(n)
    tb = t[: n - shift]
    b[shift:] = np.sin(2 * np.pi * 415.0 * tb) * np.exp(-tb / 0.052)
    sig = (a * 0.32 + b * 0.38)
    tick = click(dur=0.045, tone=1700.0, body=340.0, amp=0.45, noise_amt=0.16)
    sig[: len(tick)] += tick
    write_wav("ui_remove.wav", soften(sig, 2600.0, 0.008, 0.055), target=0.52)


if __name__ == "__main__":
    make_click()
    make_hover()
    make_select()
    make_open()
    make_close()
    make_denied()
    make_remove()
    make_elevator()
    make_dark()

# ---------------------------------------------------------------------------
# Per-theme loops. Each premium theme gets its own track so switching the theme
# actually changes the score.
# ---------------------------------------------------------------------------
NOTE_X = {"A1": 55.00, "C2": 65.41, "D2": 73.42, "E2": 82.41, "G2": 98.00, "A2": 110.00,
          "Bb1": 58.27, "B3": 246.94, "C3": 130.81, "E4": 329.63, "F3": 174.61, "F#4": 369.99, "Ab3": 207.65, "Gb3": 185.00, "Eb2": 77.78, "Eb3": 155.56, "Eb1": 38.89, "D1": 36.71, "C1": 32.70, "F5": 698.46, "F2": 87.31, "F#5": 739.99, "G#4": 415.30, "D3": 146.83, "E3": 164.81, "G3": 196.00, "A3": 220.00,
          "Bb3": 233.08, "C4": 261.63, "D4": 293.66, "F4": 349.23, "G4": 392.00, "A4": 440.00,
          "Bb4": 466.16, "C5": 523.25, "D5": 587.33, "E5": 659.26, "G5": 783.99, "A5": 880.00,
          "Bb5": 932.33, "C6": 1046.50, "D6": 1174.66}


def _pad_notes(freq, dur, amp=0.15, detune=0.0035, cutoff=2000.0, atk=0.4):
    n = int(dur * SR)
    t = np.arange(n) / SR
    sig = np.zeros(n)
    for k, g in ((1, 1.0), (2, 0.34), (3, 0.16), (4, 0.09)):
        sig += np.sin(2 * np.pi * freq * k * (1 + detune * k) * t) * g
    sig = one_pole_lp(sig, cutoff)
    a = min(n, int(atk * SR))
    sig[:a] *= np.linspace(0, 1, a)
    return sig * amp


def _bell(freq, dur, amp=0.22, cutoff=3400.0, decay=0.9):
    n = int(dur * SR)
    t = np.arange(n) / SR
    idx = 1.8 * np.exp(-t / (decay * 0.22))
    sig = np.sin(2 * np.pi * freq * t + idx * np.sin(2 * np.pi * freq * 2.01 * t))
    sig += 0.30 * np.sin(2 * np.pi * freq * 2.76 * t) * np.exp(-t / (decay * 0.12))
    sig = one_pole_lp(sig, cutoff)
    sig *= np.exp(-t / (dur * decay * 0.5))
    a = min(n, int(0.006 * SR))
    sig[:a] *= np.linspace(0, 1, a)
    return sig * amp


def _sub(freq, dur, amp=0.5, decay=0.6, drive=1.6):
    n = int(dur * SR)
    t = np.arange(n) / SR
    sig = np.sin(2 * np.pi * freq * t) + 0.22 * np.sin(2 * np.pi * freq * 2 * t)
    sig = np.tanh(sig * drive) / np.tanh(drive)
    sig *= np.exp(-t / (dur * decay))
    a = min(n, int(0.02 * SR))
    sig[:a] *= np.linspace(0, 1, a)
    return sig * amp


def _air(dur, amp=0.05, cutoff=6000.0, swell=0.5):
    n = int(dur * SR)
    t = np.arange(n) / SR
    shape = np.sin(np.pi * np.clip(t / max(1e-6, dur * swell), 0, 1)) ** 1.4
    return one_pole_lp(noise(n), cutoff) * shape * amp


def _riser(freq0, freq1, dur, amp=0.16, cutoff=3000.0):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = freq0 * (freq1 / freq0) ** (t / dur)
    ph = 2 * np.pi * np.cumsum(f) / SR
    sig = np.sin(ph) * np.exp(-t / (dur * 0.35))
    shape = np.sin(np.pi * np.clip(t / dur, 0, 1)) ** 1.2
    return one_pole_lp(sig, cutoff) * shape * amp


def make_starfield():
    bpm = 84.0
    beat = 60.0 / bpm
    bar = beat * 4.0
    bars = 8
    n = int(round(bar * bars * SR))
    L = np.zeros(n)
    R = np.zeros(n)

    def put(sig, t_sec, pan=0.0, gain=1.0):
        s = int(round(t_sec * SR))
        add_wrap(L, s, sig * gain * (1.0 - max(0.0, pan)))
        add_wrap(R, s, sig * gain * (1.0 + min(0.0, pan)))

    chords = [("Am9", ["A2", "C4", "E4", "G4"], "A1"),
              ("Fmaj7", ["F4", "A3", "C4", "E4"], "F2"),
              ("Cmaj7", ["C4", "G4", "E4", "B3"], "C2"),
              ("Gsus2", ["G3", "D4", "A3", "G4"], "G2")]
    arp = [["A5", "E5", "C6", "E5"], ["F5", "C5", "A5", "C5"],
           ["C5", "G5", "E5", "G5"], ["D5", "A5", "D6", "A5"]]

    for b in range(bars):
        t0 = b * bar
        _, notes, root = chords[b % 4]
        for i, nt in enumerate(notes):
            put(_pad_notes(NOTE_X[nt], bar * 1.05, amp=0.115, detune=0.006 * (i + 1), cutoff=1700.0),
                t0, pan=(i - 1.5) * 0.30, gain=0.95)
        put(_air(bar * 1.02, amp=0.030, cutoff=8200.0, swell=0.55), t0, pan=0.0)
        for step in range(8):
            ts = t0 + step * (beat / 2.0) + (0.055 if step % 2 else 0.0)
            put(_bell(NOTE_X[arp[b % 4][step % 4]], beat * 0.9, amp=0.17), ts,
                pan=0.28 if step % 2 else -0.28, gain=0.8)
        for bi in range(4):
            put(_sub(NOTE_X[root], beat * 0.95, amp=0.42, decay=0.55), t0 + bi * beat, gain=0.85)
        if b % 2 == 1:
            put(_riser(180.0, 900.0, beat * 2.0, amp=0.075), t0 + bar - beat * 2.0, pan=0.15)

    L = one_pole_lp(L, 11000.0)
    R = one_pole_lp(R, 11000.0)
    st = np.stack([L, R], axis=1)
    st = st / (float(np.max(np.abs(st))) or 1.0) * 0.70
    write_wav("theme_starfield.wav", st, target=1.0)
    print(f"starfield loop {bar * bars:.2f}s @ {bpm:.0f} bpm")


def make_hell():
    bpm = 72.0
    beat = 60.0 / bpm
    bar = beat * 4.0
    bars = 8
    n = int(round(bar * bars * SR))
    L = np.zeros(n)
    R = np.zeros(n)

    def put(sig, t_sec, pan=0.0, gain=1.0):
        s = int(round(t_sec * SR))
        add_wrap(L, s, sig * gain * (1.0 - max(0.0, pan)))
        add_wrap(R, s, sig * gain * (1.0 + min(0.0, pan)))

    # Locrian-ish: tritone and flat-2 for unease.
    chords = [(["D2", "D3", "F3", "Ab3"], "D1"),
              (["Eb2", "Eb3", "Gb3", "A3"], "Eb1"),
              (["D2", "D3", "G3", "A3"], "D1"),
              (["C2", "C3", "F3", "Ab3"], "C1")]
    root_map = {"D1": 36.71, "Eb1": 38.89, "C1": 32.70}
    node = {"Ab3": 207.65, "Gb3": 185.00, "A3": 220.00, "F3": 174.61, "G3": 196.00,
            "D3": 146.83, "Eb3": 155.56, "C3": 130.81, "D2": 73.42, "Eb2": 77.78, "C2": 65.41}

    for b in range(bars):
        t0 = b * bar
        notes, root = chords[b % 4]
        for i, nt in enumerate(notes):
            put(_pad_notes(node[nt], bar * 1.05, amp=0.10, detune=-0.005 * (i + 1), cutoff=900.0, atk=0.5),
                t0, pan=(i - 1.5) * 0.26)
        for bi in range(4):
            put(_sub(root_map[root], beat * 0.98, amp=0.62, decay=0.5, drive=2.4), t0 + bi * beat)
        if b % 2 == 0:
            put(_air(bar * 1.0, amp=0.075, cutoff=2200.0, swell=0.7), t0)
        put(_riser(70.0, 42.0, bar * 0.9, amp=0.13, cutoff=700.0), t0 + bar * 0.5, pan=0.0)

    L = one_pole_lp(L, 7000.0)
    R = one_pole_lp(R, 7000.0)
    st = np.stack([L, R], axis=1)
    st = st / (float(np.max(np.abs(st))) or 1.0) * 0.72
    write_wav("theme_hell.wav", st, target=1.0)
    print(f"hell loop {bar * bars:.2f}s @ {bpm:.0f} bpm")


def make_heaven():
    bpm = 60.0
    beat = 60.0 / bpm
    bar = beat * 4.0
    bars = 8
    n = int(round(bar * bars * SR))
    L = np.zeros(n)
    R = np.zeros(n)

    def put(sig, t_sec, pan=0.0, gain=1.0):
        s = int(round(t_sec * SR))
        add_wrap(L, s, sig * gain * (1.0 - max(0.0, pan)))
        add_wrap(R, s, sig * gain * (1.0 + min(0.0, pan)))

    # Lydian-tinged major pads, no percussion: a held choir-like wash.
    chords = [(["C3", "G3", "C4", "E4", "F#4"], "C2"),
              (["D3", "A3", "D4", "F#4", "G4"], "D2"),
              (["E3", "B3", "E4", "G4", "A4"], "E2"),
              (["D3", "A3", "D4", "F#4", "A4"], "D2")]

    for b in range(bars):
        t0 = b * bar
        notes, root = chords[b % 4]
        for i, nt in enumerate(notes):
            put(_pad_notes(NOTE_X[nt], bar * 1.06, amp=0.085, detune=0.0042, cutoff=1500.0, atk=0.55),
                t0, pan=(i - 2) * 0.32)
        put(_pad_notes(NOTE_X[root], bar * 1.06, amp=0.14, detune=0.002, cutoff=700.0, atk=0.6),
            t0, pan=0.0)
        if b % 2 == 1:
            put(_bell(NOTE_X["C6"], bar * 0.9, amp=0.085, cutoff=5200.0, decay=1.5), t0 + beat * 1.5,
                pan=0.2)
            put(_bell(NOTE_X["A5"], bar * 0.9, amp=0.070, cutoff=5000.0, decay=1.5), t0 + beat * 3.0,
                pan=-0.2)
        put(_air(bar * 1.04, amp=0.045, cutoff=9000.0, swell=0.6), t0)

    st = np.stack([one_pole_lp(L, 10000.0), one_pole_lp(R, 10000.0)], axis=1)
    st = st / (float(np.max(np.abs(st))) or 1.0) * 0.66
    write_wav("theme_heaven.wav", st, target=1.0)
    print(f"heaven loop {bar * bars:.2f}s @ {bpm:.0f} bpm")
