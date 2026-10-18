"""Builds Nebula's custom-voice mod: Mario's voice lines from a folder of clips (made for the
"Super Mario Odyssey Voice" sm64ex character pack: "(Mario) Jump 1.ogg" and so on).

    py -3 tools/animations/build_voice_mod.py --clips "C:/path/SMO Mario Voice/sound" --content C:/nb/build/content

Writes <content>/mods/voice: each clip as 16-bit mono WAV at 32000 Hz (the rate Galaxy's DSP mixes at),
and voices.txt, which lists for each of Galaxy's player-voice sound IDs the clips to pick from. Nebula
silences the game's line and plays one of these instead (runtime/include/galaxy/voice_replacement.h);
F1 > General > Custom voice switches it live. Needs numpy and soundfile.
"""
import argparse
import os
import struct

import numpy as np
import soundfile

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
# Loudness of the converted clips: RMS of the voiced part, with peaks capped. Like the game's own voice samples,
# they are near full scale: the game's channel volume and placement turn them down in the mix.
TARGET_RMS = 0.25
PEAK_CAP = 0.95
# Quiet lines are quieter than shouts: a factor on TARGET_RMS per clip.
SOFT = {'Sleeping 1': 0.25, 'Sleeping 2': 0.25, 'Yawning': 0.45, 'Panting 1': 0.45, 'Panting 2': 0.45,
        'Panting 3': 0.45, 'Grab Ledge': 0.8, 'Get up from Ledge 1': 0.7, 'Get up from Ledge 2': 0.7,
        'Get up from Ledge 3': 0.7, 'Lands': 0.8}


def resample(x, src, dst):
    if src == dst:
        return x
    n = int(round(len(x) * dst / src))
    spectrum = np.fft.rfft(x)
    keep = n // 2 + 1
    out = np.zeros(keep, dtype=complex)
    out[:min(keep, len(spectrum))] = spectrum[:min(keep, len(spectrum))]
    return np.fft.irfft(out, n) * (n / len(x))


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
    converted = {}
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
                mono = data.mean(1)
                mono = resample(mono, rate, RATE)
                voiced = mono[np.abs(mono) > 0.02 * (np.abs(mono).max() or 1.0)]
                rms = float(np.sqrt(np.mean(voiced ** 2))) if len(voiced) else 1.0
                gain = TARGET_RMS * SOFT.get(clip, 1.0) / (rms or 1.0)
                gain = min(gain, PEAK_CAP / (np.abs(mono).max() or 1.0))
                file = clip.lower().replace(' ', '_') + '.wav'
                write_wav(os.path.join(out, file), mono * gain)
                converted[clip] = file
            files.append(converted[clip])
        if files:
            lines.append('0x%08X %s  # %s' % (sound_id, ' '.join(files), name))
    with open(os.path.join(out, 'voices.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('%d lines, %d clips -> %s' % (len(lines) - 2, len(converted), out))


if __name__ == '__main__':
    main()
