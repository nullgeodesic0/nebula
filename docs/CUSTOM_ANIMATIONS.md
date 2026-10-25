# Custom Mario animations

Nebula can replace Mario's animations, model and voice with ones converted from the player's own files. The
converter (`tools/animations`) was built for a file holding the Super Mario Odyssey Mario rig and its actions.
Everything is built on the player's machine from their own files and their own `game.pak`. Neither the
repository nor the built game contains game data or converted content.

## Installing as a mod

The build scripts below write their output to `<content>\mods\<animations|model|voice>`. `nebula_mod.py`
packs those into one folder of Nebula's mods folder (`%LOCALAPPDATA%\Nebula\mods`, mods.h), which loads it like
any other mod: the archives as `files\ObjectData\*.arc` and the clips as `voice\`.

```
py -3 tools/animations/nebula_mod.py --out "%LOCALAPPDATA%\Nebula\mods\Odyssey Mario" ^
    <content>\mods\model <content>\mods\animations <content>\mods\voice
```

Settings > Mods turns it on or off (the disc part applies at the next start); Settings > General > Custom voice
switches the voice alone. For animations without the model, pack the animations folder alone; with the
Odyssey model, use the animations built for it (`animations-model`, with its face). Luigi's animation swap
table (`luigi_swap.txt`) is not read yet.

## Animation packs (drop-in, no build step)

An animation pack is a folder of `<animation>.ank1` files (each the ANK1 section of the MarioAnime.arc animation of
that name) and a `pack.txt`; `build_animation_mod.py --pack <folder>` on the odyssey-animations tools writes one.
Put the folder in `%LOCALAPPDATA%\Nebula\mods` as it is (or give any mod an `animations\` folder of `.ank1`
files): it shows in Settings > Mods like any mod and applies at the next start. When the mods are applied
(`runtime/src/animation_packs.cpp`), MarioAnime.arc as the other mods left it (the game's, or a model mod's) is
rebuilt with each animation a pack has, keeping the animation's own BCK header, written to
`%LOCALAPPDATA%\Nebula\generated\MarioAnime.arc` and served in its place; `mods-report.txt` says how many were
replaced. Where two packs have the same animation, the one higher in the list wins. A pack holds no game data, but
one converted from another game's animations is that game's content: keep it on the player's machine.

## Building the mod

```
py -3 -m pip install numpy matplotlib
py -3 tools/animations/build_animation_mod.py --blend "C:\path\to\file.blend" --content C:\path\to\content
py -3 tools/animations/preview.py --blend "C:\path\to\file.blend" --content C:\path\to\content --out sheet.png
```

The build takes a few seconds and writes `<content>\mods\animations`:

- `MarioAnime.arc.yaz0`: the player's animation archive with the mapped BCKs replaced
- `fst_marioanime.bin`: the disc's file-system entry for that archive, pointed at the new copy
- `manifest.txt`: a version-1 disc mod manifest
- `report.txt`: what was mapped and how it was timed

`preview.py` draws Galaxy's Mario posed by the built files (blue) over the source action (orange).

## How it works

- **Reading the .blend.** `blendfile.py` reads the file's own struct catalogue (SDNA). It needs no
  Blender install and handles Blender 5's layered actions.
- **Retargeting.** The Odyssey rig and Galaxy's Mario share joint names and local axes for the
  body. On the rest poses, the legs, spine and head agree exactly, the feet differ by 10 degrees, and
  the arms and hands differ by a fixed roll. Each mapped Galaxy joint takes the source bone's world
  rotation times a constant rest correction, and its local rotation comes from its parent's.
  Translations carry over as offsets from the rest pose. Joints are mapped by name:
  All_Root=nw4f_root, Center=AllRoot, and JointRoot, Hip, LegL/R1-2, FootL/R, Spine1-2, Head,
  ShoulderL/R, ArmL/R1-2 and HandL/R map to themselves.
- **Timing.** Loops keep the game's cycle length, with one source cycle stretched onto it. The game
  scales the playback rate with Mario's speed, so this keeps his feet in step with the ground. One-shot
  animations keep the source timing and are cut at the game's length; a shorter source is stretched to
  fit. The wall slide is a 5-frame loop in the game, so it uses the source's first frame as a held pose.
- **Writing BCKs.** Each track is fitted with as few Hermite keys as it needs (0.35 degrees, 0.12
  units). A joint whose rotation error grows near a quarter-turn pitch gets a key on every frame. The
  rotation range is chosen per file, so flips that pass a full turn still fit.
- **Loading.** The build writes the new archive with a patched file-table entry and a manifest (the
  format the recompiled runtime read). `nebula_mod.py` takes the archive under its disc path instead, and
  Nebula's virtual disc serves it in place of the game's.

## Custom model

```
py -3 tools/animations/build_model_mod.py --dae C:\path\to\mario.dae --content C:\path\to\content
py -3 tools/animations/render_model.py <built mario.bdl> out.png
```

The builder was written for the Smash Bros. Ultimate Mario rip, a skinned COLLADA mesh on a 98-bone
skeleton with PBR textures. It writes `<content>\mods\model`, a disc mod read the same way as the
animations.

- **One skeleton.** The model is re-skinned onto Galaxy Mario's 30 joints, the same skeleton the game
  animations and the converted animations drive. Scaled by 9.26, the Smash bind pose lands on Galaxy's
  rest skeleton within a few units; both are T-poses. Each source bone folds onto a Galaxy joint:
  - The spine, head, arms and legs map one to one.
  - Neck and clavicle-centre go to Spine2.
  - Fingers, face, mouth, cap and mustache go to Head or HandL/R.
  - Toes go to the feet, and twist and helper bones go to their limb.

  Vertices move with their joint's offset between the two skeletons.
- **Geometry.** The default look is body, cap, neutral face, eyes and hair: 6,835 triangles. The other
  face meshes (expressions, blinks) and the hatless head are left out.
- **Model file.** `bdl.py` writes `mario.bdl` with new VTX1, EVP1, DRW1 and SHP1 sections. The packets
  hold at most 10 matrices each. The game's JNT1 (skeleton), INF1 (scene graph), MAT3 and MDL3
  (materials) are kept, and the texture sizes in MDL3 are patched.
- **Textures.** Everything is drawn by the body material, Body1_v, which is always drawn and is the one
  the game recolours. It uses one 512x512 CMPR atlas: base colour times softened ambient occlusion.
  Normal, roughness, metal and specular maps are dropped. Cut-out cards (hair, brows) are drawn opaque,
  so their clear parts are filled with the card's colour, and triangles over fully clear texels are
  dropped.
- **Hidden parts.** Galaxy's face, hand and cap/hair part models are kept but draw nothing, because
  the new mesh has its own.

What does not match:

- **No expressions.** There are no facial expressions, blinking or hand poses (fist, open, holding),
  and the eyes do not move.
- **One look for every form.** The body textures that the game swaps for Fire Mario and others all
  show the atlas. Bee, Boo, Ice, Spring and Rainbow Mario use their own models and stay Galaxy's.
- **Lighting.** It is the game's per-vertex lighting on a flat colour texture, not physically based.
- **Not yet seen in game.** The written model has only been checked by decoding it and rendering its
  rest pose.

## Odyssey model with face joints

```
py -3 tools/animations/build_odyssey_model_mod.py --blend mario-model-FK.blend --textures <folder> --anim-blend <animation .blend> --content <content>
py -3 tools/animations/build_animation_mod.py --blend <animation .blend> --content <content> --model <content>\mods\model --face-anims PlayerAnimationFace.szs --face-skeleton "Custom Bone vis Armature.fbx"
```

- **Skeleton.** `build_odyssey_model_mod.py` keeps Galaxy's 30 joints and appends 70: Odyssey's face
  bones and the rig's eyelid bones under Head, and the finger bones under HandL/R. Their rest frames come
  from the animation rig, so the converted actions drive them like any other joint.
- **64 draw matrices.** Galaxy draws Mario through `J3DModelX` and computes his draw matrices for each view
  into "extra" buffers of 0xC00 bytes, 64 matrices (`initExtraMtxBuffer` in Petari's MarioActorDraw.cpp).
  A model's draw matrices are its joints that hold vertices of their own plus its blends (DRW1 count minus
  EVP1 count); the game's model has 35. Past 64 the buffers overrun: vertices take neighbouring matrices
  (long spikes toward the feet or the T-pose, overbright patches from bad normal matrices) and, far enough
  over (187 blends), memory is corrupted and the game crashes in `DisplayListMaker::update`. So:
  - small face bones give their vertices to a neighbour (`MERGE`: tongue and lower teeth to Jaw, upper teeth
    to MouthRoot, cheeks, cheek puffs and lower lids to Head, outer brows to the brow centres, outer mustache
    to its root, eyelid helpers to the main lid);
  - the fingers keep their joints but no vertices (`UNSKINNED`). Their geometry is posed once in the idle's
    first frame (world rotations taken from the animation rig, so the two rigs' bone rolls do not matter)
    and bound to the hand: a relaxed hand rather than the straight rest pose;
  - body vertices blend two bones in quarter steps, face vertices in half steps. Blends are kept in order
    (the body's half blends, quarter blends around the hips, then the face's) while joints plus blends stay
    within 64 (`--draw-matrices`), counted exactly; a dropped quarter blend falls back to its pair's half
    blend. Currently 32 joints and 32 blends, 10 of them on the face.
  - Positions and normals are not shared between draw matrices, as in the game's own model.
  - Triangles are written in GX's winding (the game's normals point against their counter-clockwise
    order). Wound the other way, the model looked fine in the main pass but was drawn inside out by the
    game's back-face-culling passes, and Mario turned silver against walls.
- **One face mesh.** Odyssey switches between nine face meshes (Face00-08, by bone-visibility tracks) and
  bends the shown one; this model has a single face, so every mouth shape comes from bending it.
  `FACE_STRENGTH` tones down the run's wide-open jaw, which stretched the chin.
- **Power-up models.** Galaxy plays Bee, Ice, Invincible, Hopper and Bone Mario with the main model's
  animation player, which is sized to the main model's joints; with only their own 30 it wrote past them and
  the game crashed on taking a Bee Mushroom. Their archives get the same 70 added joints, with no geometry
  (`FORMS`, `bdl.with_joints`).
- **Game animations.** Only the converted animations carry tracks for the new joints; extending all 456
  game animations (5.1 MB unpacked) panics in HeapMemoryWatcher. J3D reads a joint past the end of an
  animation's table without a check, which flung the face and fingers into spikes during the spin and
  other game animations. Nebula now intercepts `J3DAnmTransformKey::getTransform` (0x80440CDC; its only
  caller of `calcTransform`) while the custom model is on. A joint at or past the animation's joint count
  (u16 at +0x1E) gets the neutral pose that `build_animation_mod.py --model` writes to
  `mods/model/neutral_joints.txt` (the idle face, relaxed hands). `GALAXY_TRACE_ANIM_GUARD=1` traces it.
  Build the model first, then the animations, since the model build replaces that folder.
- **Face animation** (`face_anim.py`, `bfres.py`, `fbx.py`). Odyssey animates the face as a separate
  skeleton rooted at `Face`. `PlayerAnimationFace.szs` holds 440 skeletal animations (Switch BFRES 0.8,
  Euler XYZ, curve-compressed) named after Odyssey's actions; the FBX supplies that skeleton's rest pose.
  For each converted action, the face animation of the same name (`FACE_ACTION` lists the exceptions)
  is sampled on the body's timing. Each bone's motion relative to Face is carried into Head's frame by
  the rotation and scale that best align the two rest skeletons (scale 0.999, 180 degrees about X), then
  turned back into local transforms for the model's own hierarchy. Every other animation shows the
  first frame of Odyssey's idle face. Blinks and closed eyes come from the rig's eyelid bones in the
  animation .blend (the idle blinks once per loop); the face file's bone-visibility tracks are not used.

### Odyssey Luigi (for co-op)

```
py -3 tools/animations/build_odyssey_luigi_mod.py --gltf Luigi.gltf --content <content>
```

Run after the Mario model build on the same content; it appends `Luigi.arc` and Luigi's power-up models
(Bee, Ice, Invincible, Hopper, Bone) to `mods/model/manifest.txt`. Galaxy's Luigi has Mario's 30 joints and
plays Mario's animations, so the Luigi model takes the Mario model's whole 100-joint skeleton (its added
joints read from the built `Mario.arc`). The glTF (`gltf.py`) is Odyssey's Luigi on the Odyssey rig, with
no face rig or finger bones: the face, cap and fingers ride on Head and the hands, and his face does not
animate. The animations set every joint's offset, so each limb and spine segment of the mesh is stretched
along its bone onto Galaxy's joint spacing (legs 0.70 to 0.84, arms up to 1.42, neck 1.33); across the bone
one overall scale holds. The export wrote its normals Z-up while the positions are Y-up; the builder picks
the frame in which the normals agree with the triangles' winding. glTF's texture v runs downward, Blender's
up. The closed eyelids are left out; the cap-off hair stays (it is also the hair below the cap). The atlas
is opaque, and Luigi's pupil sheet covers the whole eye but is about 82% see-through around the iris, so it
is composited over an eye white instead of filled with its average colour, which made the eyes solid blue
(Mario's pupil sheet is opaque and needs neither).

## Voices (Settings > General > Custom voice)

`build_voice_mod.py` converts the SMO voice clips to 16-bit mono 32 kHz WAVs in `<content>\mods\voice`,
with `voices.txt` mapping Galaxy player-voice sound IDs to clips. Each clip is resampled with a
Kaiser-windowed sinc (`audio.py`), brought to -12 LUFS (ITU-R BS.1770; the quiet lines keep their offsets in
`SOFT`) and held under -1 dBTP by a look-ahead limiter; `report.txt` lists every clip's loudness and true
peak. Ten sources (the jumps, double jumps and electrocutions) peak 2-4 dB over full scale: their
distortion is in the files. At runtime (`voice_replacement.cpp`, `jaudio_host_mixer.cpp`) a replaced line's
channel plays the clip instead of the game's samples, at the clip's own level, so the game's volume, 3D
placement, filters and effect sends apply. (Matching each clip to the energy of the game's own line made the
same clip swing by 8 dB between plays and clipped it.) When the game's line ends or its channel fades well
below its loudest, the clip finishes through the channel's loudest state. The spin and the flips
(SE_PV_JUMP_TURN) keep Galaxy's voices. `GALAXY_TRACE_VOICE=1` logs every line and how each clip played.

## Mapping (tools/animations/build_animation_mod.py `MAP`)

| Galaxy | Source action | Notes |
| --- | --- | --- |
| walk, walksoft | Walk, WalkSoft | loop |
| run, rundash | Run, Dash | loop; a 16-frame source cycle on the game's 60 |
| wait | Wait oddysey idle | loop, 180 frames each |
| jump, jump2, jumproll (3rd jump) | Jump, Jump2, Jump3 | body lift x0.6, 0.6, 0.5 |
| jumpback (backflip), jumpbroad | JumpBack, JumpBroad | |
| fall, land | Fall, Land | |
| walljump, wallslide, wallkeep | WallJump, WallSlideL (held pose), WallKeep | |
| hipdropstart, hipdrop, hipdropland | HipDropStart, HipDrop, HipDropLand | ground pound |
| squatstart, squatwait, squatwalk, squatend | Squat* | |
| swimwait, swimbreast, swimflutter | SwimWait, SwimWalk, SwimWalk | underwater |
| swimbreastsurface, swimfluttersurface | SwimSurface | surface |

## What does not match

- **Joints the source does not have.** Galaxy's face, eye and cap anchors, the hand attachment
  joints and PartsControl keep the game's own tracks. Hand poses (fist or open) come from Galaxy's
  separate hand models, not from the source's fingers. Odyssey's toes, fingers, face and cap bones
  are dropped.
- **Proportions.** Galaxy's shoulders sit about 3 units lower than Odyssey's, so the hands end up a
  little lower than in the source.
- **Timing and lift.** The game's animation lengths and playback rates are kept. Long one-shots
  (Odyssey's 84-frame Jump) are cut to the game's length (24 frames). Jump animations carry only part
  of the source's vertical body motion, because the game's physics moves Mario.
- **Swimming.** Odyssey has no underwater breaststroke separate from its flutter kick, so both use
  SwimWalk, and both surface swims use SwimSurface. The game rotates Mario for swimming, as Odyssey
  does.
- **Not converted.** Spin, damage, poles, ledges, cutscenes and everything else keep the game's
  animations. The side flip (jumpturn) is left out because the file's JumpTurn has keys for only 38
  bones. Add rows to `MAP` to convert more.
- **Facial animation.** BTP and BTK texture animations (eyes, mouth) are the game's.
