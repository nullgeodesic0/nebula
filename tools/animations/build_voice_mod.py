"""Builds Nebula's custom-voice mod: Mario's voice lines from a folder of clips (made for the
"Super Mario Odyssey Voice" sm64ex character pack: "(Mario) Jump 1.ogg" and so on).

    py -3 tools/animations/build_voice_mod.py --clips "C:/path/SMO Mario Voice/sound" --content C:/nb/build/content

Writes <content>/mods/voice: each clip as 16-bit mono WAV at 32000 Hz (the rate Galaxy's DSP mixes at;
band-limited resampling), loudness-normalised with a true-peak ceiling (report.txt),
and voices.txt, which lists for each of Galaxy's player-voice sound IDs the clips to pick from. Nebula
silences the game's line and plays one of these instead (runtime/include/galaxy/voice_replacement.h);
F1 > General > Custom voice switches it live. Needs numpy and soundfile.
"""
import argparse
import os
import struct
import sys

import numpy as np
import soundfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio  # noqa: E402

# Galaxy player-voice sound (AudioRes/SMR.szs sound name table, section 0, PLAYER_VOICE) -> clip names
# (without "(Mario) " and ".ogg"). Lines not listed keep the game's voice (the spin and flips keep Galaxy's).
LINES = {
    0x10000: ('SE_PV_JUMP_S', ['Jump 1', 'Jump 2', 'Jump 3', 'Jump 4', 'Jump 5']),
    0x10001: ('SE_PV_JUMP_M', ['Double Jump 1', 'Double Jump 2', 'Double Jump 3']),
    0x10002: ('SE_PV_JUMP_L', ['Triple Jump 1', 'Triple Jump 2']),
    # SE_PV_JUMP_TURN (0x10003, the flips) keeps Galaxy's voice: Odyssey's flip clips are too short.
    # Galaxy has no long jump: JUMP_LONG is the backflip (crouch, then jump), measured with GALAXY_TRACE_VOICE.
    0x10004: ('SE_PV_JUMP_LONG', ['Long Jump 1', 'Long Jump 2', 'Long Jump 3']),
    0x10005: ('SE_PV_JUMP_JOY', ['Triple Jump 1', 'Triple Jump 2']),
    0x10006: ('SE_PV_LAND', ['Lands']),
    0x10007: ('SE_PV_HIP_DROP', ['Ground Pound 1', 'Ground Pound 2']),
    0x10008: ('SE_PV_DAMAGE_S', ['Hurt 1', 'Hurt 2', 'Hurt 3']),
    0x10009: ('SE_PV_DAMAGE_L', ['oof 1', 'ooof 2']),
    0x1000A: ('SE_PV_PUNCH', ['Punch 1', 'Punch 2']),
    0x1000B: ('SE_PV_KICK', ['Kick 1', 'Kick 2']),
    0x1000D: ('SE_PV_LAST_DAMAGE', ['Dying 1', 'Dying 2']),
    0x1000E: ('SE_PV_DOWN', ['ooof 2']),
    0x10010: ('SE_PV_THROW', ['Throw Bowser 1', 'Throw Bowser 2']),
    0x10012: ('SE_PV_CATCH', ['Grab Ledge']),
    0x10017: ('SE_PV_BURN', ['Hurt in Lava 1', 'Hurt in Lava 2', 'Hurt in Lava 3']),
    0x10018: ('SE_PV_FALL_DIE', ['Falling']),
    0x10019: ('SE_PV_ELEC_DAMAGE', ['Electrocuted 1', 'Electrocuted 2']),
    0x1001A: ('SE_PV_BURN_RUN', ['Hurt in Lava 1', 'Hurt in Lava 2', 'Hurt in Lava 3']),
    0x1001F: ('SE_PV_YAWN', ['Yawning']),
    0x10020: ('SE_PV_SLEEP_1', ['Sleeping 1']),
    0x10021: ('SE_PV_SLEEP_2', ['Sleeping 2']),
    0x10026: ('SE_PV_THROWN', ['Thrown']),
    0x10031: ('SE_PV_LAND_COOL', ['Lands']),
    0x1003B: ('SE_PV_DAMAGE_S_WATER', ['Hurt 1', 'Hurt 2', 'Hurt 3']),
    0x1003C: ('SE_PV_LAST_DAMAGE_WATER', ['Drowning 1', 'Drowning 2']),
    0x10041: ('SE_PV_CLIFF_FALL_HANG', ['Grab Ledge']),
    0x10042: ('SE_PV_CLIFF_CLIMB', ['Get up from Ledge 1', 'Get up from Ledge 2', 'Get up from Ledge 3']),
    0x10043: ('SE_PV_BURY_HEAD', ['Inside The Ground 1', 'Inside The Ground 2']),
    0x10045: ('SE_PV_BURY_FOOT', ['Inside The Ground 1', 'Inside The Ground 2']),
    0x1004A: ('SE_PV_POWER_STAR_GET', ['Star Get 1', 'Star Get 2']),
    0x1004B: ('SE_PV_GRAND_STAR_GET', ['Star Get 1', 'Star Get 2']),
    0x1004C: ('SE_PV_TIRED', ['Panting 1', 'Panting 2', 'Panting 3']),
    0x10053: ('SE_PV_WALL_HIT_BODY', ['oof 1']),
}
RATE = 32000
# Every clip is brought to one loudness (ITU-R BS.1770, LUFS) and its true peak held under the ceiling by a
# look-ahead limiter. Nebula plays the clips at that level on the game's voice channel, whose volume and
# placement the game still applies (voice_replacement.cpp). The sources range from -37 to -6 LUFS, and ten
# of them (the jumps, double jumps and electrocutions) peak 2-4 dB over full scale.
TARGET_LUFS = -12.0
CEILING_DBTP = -1.0
# Quiet lines are quieter than shouts: a factor on the target's level per clip.
SOFT = {'Sleeping 1': 0.25, 'Sleeping 2': 0.25, 'Yawning': 0.45, 'Panting 1': 0.45, 'Panting 2': 0.45,
        'Panting 3': 0.45, 'Grab Ledge': 0.8, 'Get up from Ledge 1': 0.7, 'Get up from Ledge 2': 0.7,
        'Get up from Ledge 3': 0.7, 'Lands': 0.8}
FADE_IN, FADE_OUT = 0.001, 0.005  # seconds: no click where a clip starts or stops mid-wave


def limit(x, rate, ceiling, lookahead=0.0015, release=0.06):
    """Look-ahead peak limiter: the gain falls over the look-ahead window before a peak over the ceiling
    (measured 4x oversampled) and recovers over the release, so attacks keep their shape."""
    over = np.abs(audio.resample(x, rate, rate * 4, half_taps=24))
    over = np.pad(over, (0, (-len(over)) % 4)).reshape(-1, 4).max(1)[:len(x)]
    need = np.minimum(1.0, ceiling / np.maximum(over, 1e-9))
    ahead = max(1, int(lookahead * rate))
    held = np.array([need[i:i + ahead].min() for i in range(len(need))])
    gain = np.empty_like(held)
    g = 1.0
    rel = np.exp(-1.0 / (release * rate))
    for i, h in enumerate(held):
        g = h if h < g else h + (g - h) * rel
        gain[i] = g
    # Eased across the look-ahead, never above what the peaks need.
    gain = np.minimum(gain, np.convolve(np.pad(gain, (ahead, ahead), mode='edge'), np.ones(ahead) / ahead,
                                        mode='same')[ahead:-ahead])
    return x * gain


def write_wav(path, samples):
    data = np.clip(np.round(samples * 32767.0), -32768, 32767).astype('<i2').tobytes()
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b'data' + struct.pack('<I', len(data)) + data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--clips', required=True)
    ap.add_argument('--content', required=True)
    args = ap.parse_args()
    out = os.path.join(args.content, 'mods', 'voice')
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith('.wav') or f == 'voices.txt':
            os.remove(os.path.join(out, f))  # clips of lines no longer replaced (the flips) go too
    converted = {}
    report = []
    lines = ['# Nebula custom voice, built by tools/animations/build_voice_mod.py',
             '# sound id, then the clips to pick from']
    for sound_id, (name, clips) in sorted(LINES.items()):
        files = []
        for clip in clips:
            src = os.path.join(args.clips, '(Mario) %s.ogg' % clip)
            if not os.path.exists(src):
                print('missing', src)
                continue
            if clip not in converted:
                data, rate = soundfile.read(src, always_2d=True)
                mono = audio.resample(data.mean(1), rate, RATE)
                target = TARGET_LUFS + 20 * np.log10(SOFT.get(clip, 1.0))
                # The limiter takes some loudness off peaky clips: normalise and limit again until on target.
                for _ in range(12):
                    short = target - audio.loudness(mono, RATE)
                    if abs(short) < 0.1:
                        break
                    mono = limit(mono * 10 ** (short / 20), RATE, 10 ** (CEILING_DBTP / 20))
                fade_in, fade_out = int(FADE_IN * RATE), int(FADE_OUT * RATE)
                mono[:fade_in] *= np.linspace(0.0, 1.0, fade_in, endpoint=False)
                mono[-fade_out:] *= np.linspace(1.0, 0.0, fade_out)
                file = clip.lower().replace(' ', '_') + '.wav'
                write_wav(os.path.join(out, file), mono)
                converted[clip] = file
                report.append((clip, rate, mono))
            files.append(converted[clip])
        if files:
            lines.append('0x%08X %s  # %s' % (sound_id, ' '.join(files), name))
    with open(os.path.join(out, 'voices.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    with open(os.path.join(out, 'report.txt'), 'w', newline='\n') as f:
        f.write('clip                     source Hz   LUFS  true peak dBTP\n')
        for clip, rate, mono in report:
            f.write('%-24s %9d %6.1f %8.1f\n' % (clip, rate, audio.loudness(mono, RATE),
                                                  20 * np.log10(max(audio.true_peak(mono, RATE), 1e-9))))
    print('%d lines, %d clips -> %s' % (len(lines) - 2, len(converted), out))


if __name__ == '__main__':
    main()
