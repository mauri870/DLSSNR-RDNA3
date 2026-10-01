DLSSNR-AMD-Vulkan (Linux)
=========================

Runs the neural rendering (NR) model of DLSS 5 in games on AMD graphics cards, under Linux + Proton.
The network is reimplemented in Vulkan and runs on the game's own Vulkan device; NVIDIA's
runtime is neither needed nor called.

Requirements
------------
- RX 9000 series (RDNA4) graphics card; or RX 7000 series (RDNA3) with the package whose name ends in -rdna3
  (about five times slower than RDNA4: see README-RDNA3.md in the source tree)
- Mesa 26.2 or newer (RADV driver)
- Proton: tested on GE-Proton 11-11
- python3 (the installer and the model extraction use it)

Install
-------
    bash install.sh "/path/to/steamapps/common/<game folder>" --dll /path/to/nvngx_dlssnr_2026_10_16.zip

The folder is the one that holds the game's exe (the one that actually runs - for Unreal Engine
games it is <project>/Binaries/Win64, not the top folder with the launcher). The script lists
the routes; you can also name one after the folder:

    optiscaler             for games with a DLSS, FSR or XeSS option. OptiScaler does the upscaling and
                           frame generation (the original OptiScaler, its files unchanged); this project's
                           NR runs as an OptiScaler plugin, on D3D12, D3D11 and Vulkan games. 64-bit package
                           only.
    optiscaler-renodx      the earlier name, now the same as optiscaler (the RenoDX style is the default).
    reshade-feeder         for D3D10/11/12 games without a usable upscaler. ReShade + VORT (or the
                           optional LumeniteFX) provide the motion vectors.
    reshade-feeder-vulkan  Vulkan games, or D3D9 games (under Proton, DXVK turns D3D9 into Vulkan).
    reshade-feeder-dx9     as reshade-feeder-vulkan, plus the depth direction for old D3D9 games.
    remove                 uninstall.

The earlier route names reshade, vulkan and dx9 are now reshade-feeder, reshade-feeder-vulkan and
reshade-feeder-dx9.

When it is done the script prints the line for the Steam launch options, for example:

    WINEDLLOVERRIDES="dxgi=n,b" %command%

On the optiscaler route this is the whole line; no Vulkan layer is needed (the NR plugin draws NR
Control itself).

Use the i686 package for 32-bit games and the x86_64 package for 64-bit games.

Networks: all four are installed, named as in NVIDIA's DLL (CC_SMALL_FP8_EXPERIMENTAL is this project's):
    CC_SMALL_FP8                   the default network of 310.9.16 (default)
    CC_SMALL_FP8_EXPERIMENTAL      experimental: CC_SMALL_FP8 made faster
    CC_SILVER_AARDWOLD             the network of 310.8.0 (also preset 1 of 310.9.16)
    CC_SILVER_AARDWOLD_INT4_MIXED  CC_SILVER_AARDWOLD with part of the work in int4 and other lower
                                   precisions; faster, the picture differs somewhat
The installer asks once which one a game starts with (Enter = CC_SMALL_FP8; not asked when not run in
a terminal), or give --model <name> (the earlier --new-model, --old-model and --int4 still work;
--no-int4 is no longer needed). This only sets [Network] Model in dlssnr-amd.ini; the network can be
switched in game at any time: the drop-down in NR Control (End) or on the Add-ons page, or Ctrl+F11 to
cycle. The new network is prepared in the background, and the current one keeps running until it is
ready. A network that cannot run is greyed out in the drop-down with the reason.
CC_SILVER_AARDWOLD_INT4_MIXED needs a Vulkan layer on the reshade-feeder routes: besides the main
launch line the installer always prints a second one with that layer (VK_ADD_LAYER_PATH and
VK_INSTANCE_LAYERS added); use that line to use it, and copy the whole line. When games start with it,
the main line is that one. The optiscaler route needs no layer (the NR plugin adds the device
extensions it needs). Its weights are made at install; if that fails the other networks are installed
and it is greyed out in the menus. The first time it is used its network takes longer to build, and
until then the picture goes without NR.
CC_SMALL_FP8_EXPERIMENTAL's weights are made at install too (from CC_SMALL_FP8's model), so it also
needs the 310.9.16 nvngx_dlssnr.dll; without it, it is greyed out in the menus.

LumeniteFX (optional, reshade-feeder routes only): motion vectors from LumeniteFX Kernel (by Afzaal
(Kaidō), https://github.com/umar-afzaal/LumeniteFX, AGNYA licence). It is not part of this project and
not in this package. With --lumenite the installer downloads the version this package was tested with
from its author's GitHub and installs it unchanged into the game folder (the downloaded zip is kept in
this package's lumenite/, so later installs do not download again); or download it yourself (Code >
Download ZIP on its GitHub page) and install with --lumenite <zip>, which installs the version in the
zip. Without either, the installer asks once (Enter = no).
After installing, Motion vectors on the Add-ons page switches between LumeniteFX Kernel and VORT
(ReShade compiles its effects again, and until that is done the picture goes without NR; NR's history
starts again); it is MotionVectors in dlssnr-amd.ini. At every game start the add-on checks that
LumeniteFX works (compiled, on, in order, its flow textures, its motion vectors); if anything fails,
VORT runs that time, the reason is on the Add-ons page and in dlssnr-amd.log, and the next start tries
again.

The model
---------
The weights are NVIDIA's and are not distributed with this project. They are extracted from your
own copy of nvngx_dlssnr.dll, version 310.9.16 or 310.8.0:

    dlssnr-amd/dlssnr-small.bin  CC_SMALL_FP8 (the default network of 310.9.16), also the source of
                                 CC_SMALL_FP8_EXPERIMENTAL
    dlssnr-amd/dlssnr.bin        CC_SILVER_AARDWOLD (the network of 310.8.0; 310.9.16 has it too), also
                                 the source of CC_SILVER_AARDWOLD_INT4_MIXED

- use 310.9.16 when you can: it already contains the 310.8.0 network, so with it both models are
  extracted and installed; 310.8.0 gives CC_SILVER_AARDWOLD only (CC_SMALL_FP8 and
  CC_SMALL_FP8_EXPERIMENTAL are then greyed out in the menus);
- give --dll either the DLL itself or a zip that contains exactly one nvngx_dlssnr.dll
  (it may sit in a subfolder of the zip);
- a DLL of any other version is refused before anything is installed;
- every extracted entry (599 for CC_SILVER_AARDWOLD, 255 for CC_SMALL_FP8) is checked against known
  hashes, and a model file is written only if all of them match. This takes about 40 seconds for
  310.9.16 and 20 seconds for 310.8.0.

The result is dlssnr-amd/dlssnr-small.bin (113,917,248 bytes) and dlssnr-amd/dlssnr.bin
(147,756,560 bytes) in the game folder.

This is needed only once: the extracted models are also kept in this package's dlssnr-amd/,
and later installs from this package without --dll check their SHA256 and install them from
there. With a new package, use --dll once more, or copy those two files over.

To make the model files without installing anything (into an existing folder):

    bash model-tools/extract_model.sh /path/to/nvngx_dlssnr_2026_10_16.zip <folder>

It prints "599 entries, 140.9 MiB" for dlssnr.bin and "255 entries, 108.6 MiB" for
dlssnr-small.bin when it succeeds. The files can then be copied to <game folder>/dlssnr-amd/ by hand.

After installation the game folder has
--------------------------------------
    dlssnr-amd/                  models and shaders; the run-time pipeline cache goes here too; on the
                                 OptiScaler route also the wilsjo2 style's shaders (shaders/wilsjo2) and
                                 nr_present_probe.exe, which NR Control uses
    dlssnr-amd.ini               settings, written at install
    dlssnr-amd-install.txt       the installation record, which remove deletes by
plus the route's own files: on the OptiScaler route dxgi.dll (that is OptiScaler.dll), OptiScaler.ini
and OptiScaler/ (the NR plugin is OptiScaler/plugins/dlssnr-amd.asi, OptiScaler's licences are in
OptiScaler/Licenses/); on the ReShade routes ReShade's DLL, ini files and shaders.
Screenshots of the RenoDX style go to NR Screenshots/ (made at the first screenshot, not removed by
uninstall).

Installing again (another route, another package) first uninstalls by
the installation record, and dlssnr-amd.ini is replaced by a fresh default file: earlier settings are
not kept, back the file up if you need them.

Use
---
optiscaler: turn on DLSS, FSR or XeSS in the game; Insert opens the OptiScaler menu (upscaling and
            frame generation only).
            End opens NR Control, this project's NR menu, which holds every NR setting:
            - At the top, what NR is doing: one line a layer (processing, where it runs, network, size,
              passes, cost - split into the network itself and the processing around it; the column
              headers explain on hover), then the sums; video memory (this project's by network and
              use, and what the whole process uses); Video memory, in detail opens the breakdown.
            - NR chain: NR is one or more layers, each with its own processing (the RenoDX or the
              wilsjo2 style, see below), network ([Network] Model or one of its own) and place (after
              the upscaler, before it, Present); the layers' settings are independent. Layers at one
              place run in layer order; the places run before the upscaler, after it, then at Present.
              Every layer runs the network once more.
            - Each layer's page: all its settings, and Engine data (what the layer gets, see below).
            - Global: first Preprocess (open by default, see Preprocess below; coloured text hints at it
              when layers run before or after the upscaler or the game hands over no exposure), then the
              NR switch (F6), screenshots (F5), menu size and colours; Copy a report under Diagnostics
              copies the layers' state, video memory, dlssnr-amd.ini and the log's last lines to the
              clipboard and saves them as dlssnr-amd\nr-report.txt (nothing is sent anywhere).
            - Compare ([Inspect] in dlssnr-amd.ini): NR off against NR on, on the screen - side by side
              or with a split line (sides can be swapped, NR off / NR on tags), on the final picture or
              one layer; with Apply NR off the layers run as usual but their result is not shown. For a
              chosen layer also: the network's input, its output, its change x20 (Debug view), the skin
              weight (wilsjo2 layers), a held frame (the layer keeps processing one frame, for tuning),
              the spatial compression's work area and centre band (wilsjo2 layers).
            Changes are written to dlssnr-amd.ini at once and apply in the game within about a second
            (except those marked as applied after a game restart). While the menu is open it takes the
            keyboard and mouse; End or Esc closes it. Not yet tested on Wayland
            (PROTON_ENABLE_WAYLAND=1).
            While the menu is closed it draws nothing (except the few seconds the network's name shows in
            a corner, and the tags and spatial compression frames turned on in Compare).
            Present: NR runs on the game's finished picture, before OptiScaler's frame generation (the
            generated frames are made from the picture after NR).
            If NR Control keeps waiting for the game's upscaler, set [Spoofing] Dxgi=false in
            OptiScaler.ini and try again (Dying Light: The Beast has needed it).
            D3D11 and Vulkan games have the three places too. A layer placed at Present runs only at
            Present, never moved before or after the upscaler; when Present cannot serve it (for example
            a screen colour format it does not support), the layer does not run and its line in the menu
            says why.
reshade-feeder routes: Home opens ReShade; the NR settings are on the Add-ons page, laid out as in NR
            Control:
            - At the top, what NR is doing: one line a layer (processing, network, size, passes, cost -
              split into the network itself and the processing around it; the column headers explain on
              hover), then the sums; whether DLSS5_Feed and its motion vectors and depth were found;
              video memory, with Video memory, in detail for the breakdown.
            - NR chain: NR is one or more layers, each with its own processing (the wilsjo2 style = these
              routes' original processing, or the RenoDX style, see below), network ([Network] Model or
              one of its own) and settings, run on the picture in layer order (on these routes every
              layer runs at Present); the format is that of the optiscaler route's NR chain. Every layer
              runs the network once more; layers with different settings each take their own network
              memory.
            - Each layer's page: all its settings, and Engine data (what the layer gets, see below).
            - Global: Preprocess (first, open by default), the NR switch (F6), screenshots (F5, taken by
              the RenoDX style's layers), Diagnostics (Copy a report, as in NR Control), the network and
              other settings, Motion vectors (VORT or LumeniteFX Kernel), Log.
            - Compare: as NR Control's Compare (side by side can be zoomed; Debug view works on layers
              of both styles, Skin mask on wilsjo2 layers).
            Changes are written to dlssnr-amd.ini in the game folder at once and apply (except those
            marked as applied after a game restart). A dlssnr-amd.ini without [LayerN] sections (as the
            installer writes it) has one layer: the RenoDX style with [RenoDX] Enabled = 1 (the [RenoDX]
            settings), else the wilsjo2 style (the [DlssNr] settings); its first change on the page
            turns it into a [Layer1] section, after which these NR settings in [DlssNr] / [RenoDX] no
            longer apply; removing every layer goes back to them.
            Model resolution (WorkingScale) and Passes of 5 or more rebuild the network; for the few
            seconds of a rebuild the picture does not go through NR. Switching Passes between 1 and 2 or
            more also rebuilds in the background; until it is done NR keeps running with the previous
            number of passes.
            New noise every frame: the network's noise seed changes every frame, as in NVIDIA's original.
            Every layer on both routes has it. RenoDX layers (NRNoiseEveryFrame): on by default; off
            uses the same seed every frame. wilsjo2 layers (NoiseEveryFrame): only on frames without
            motion vectors (for example on the ReShade route while DLSS5_MV is not bound); on frames with
            motion vectors the seed always changes every frame; on by default on the OptiScaler route,
            off on the ReShade routes (as these routes always had it).
            Without motion vectors there is no previous result to average the noise, so with it on a
            still picture shimmers slightly. With Apply model off the network runs as usual (same cost)
            but its result is not shown, to compare.
            Below 100% Model resolution, Enlargement (Transfer) chooses how the model's result is
            enlarged back to full resolution: Matched residual (default) and Edge-aware lighting +
            colour enlarge the model's change and apply it to the full-resolution picture; Classic
            enlarges the model's output picture directly (scalers to choose from, FSR 1 among them);
            Bilinear and Lighting + colour are OptiScaler DLSS-NR's two ways. Model resolution goes up
            to 200% (supersampling: the picture is enlarged for the network and the result made small
            again; Supersampling filter chooses the filter). Passes 1..3, up to 30 with Unlock passes.
            wilsjo2 layers use OptiScaler DLSS-NR's key names: spatial compression
            (SpatialCompression), HDR mapping (ReversibleMode, with the modes that use the network's
            result directly and ReplaceDetailStrength), skin protection (SkinProtection and the rest).
            The Transfer numbers are the same in [DlssNr] and [LayerN]: 0 Bilinear, 1 Matched residual,
            2 Lighting + colour, 3 Edge-aware lighting + colour, 4 Classic.
            The page shows the names; no numbers to remember.
            HDR: scRGB (linear) and HDR10 (PQ) are supported; HLG pictures are not handled on these
            routes, set the game to SDR, scRGB or HDR10.

Networks and other settings ([Network] in dlssnr-amd.ini; in NR Control and on the Add-ons page under
Network model and options):
  - Residency: what is kept of a network that is no longer running. The networks share one activation
    memory, so what is kept is mainly the weights.
        auto      only the weights stay in video memory, switching back is quick; when video memory
                  runs short they move to RAM, and are freed when RAM runs short too (default)
        resident  the whole network stays in video memory; switching back is instant, uses the most
                  video memory
        offload   the weights move to RAM and all video memory is freed; switching back copies them
                  back first
        minimal   video memory and RAM are freed; switching back builds the network again
    With NR off for more than 10 seconds, offload and minimal treat the running network so too; auto
    only when video memory runs short.
  - OSD: the running network shown in a corner of the screen (one line a layer with several layers).
    switch (default) = while switching, with the progress, then for 3 seconds; always = all the time;
    cost = all the time, with each layer's cost split into the network and its processing, and the
    sums; off = never.
  - Resize History (default 0): what happens to NR's previous result when the game's render size
    changes (dynamic resolution); only matters with NR before the upscaler. 0 = dropped, NR starts
    again at the new size, as NVIDIA's DLL does; 1 = scaled to the new size and kept. In our tests 1
    is steadier and closer to the picture at a constant size when the size changes often, but it is
    not what NVIDIA's DLL does; most DLSS games jitter their input, which hides most of the flicker of
    a restart, so the default is 0.
  - Style Grade (default 1): the grade of the Natural and Cinematic styles. 1 = as NVIDIA's DLL, the
    picture is graded for the style after the network; 0 = the picture of 0.0.6 and earlier (without
    that step), for those who prefer it. Standard is not affected; applied at once.
  - Motion Without Depth (default 1): 1 = the network fetches the previous frame with the pixel's own
    motion vector, depth not used, as NVIDIA's DLL does; 0 = with depth, the motion vector of whichever
    of the pixel and its four diagonal neighbours is nearest the camera. Only the edges of moving
    objects change; applied after a game restart.
  - Hotkey, Sound: the hotkey that cycles the networks (default Ctrl+F11) and its sound.
  - [Log]: whether dlssnr-amd.log is written and whether it is cleared at every start; applied after a
    game restart.
Dynamic resolution: with NR before the upscaler and a render size that keeps changing, every new
size is prepared in the frame it appears in (except the first range of sizes seen), so NR is no
longer skipped for stretches of frames; its video memory is bounded.
Engine data (each layer's page in NR Control and the Add-ons page): what the network actually gets,
for troubleshooting - the colour format and HDR, the encoding the network sees, the exposure (who
computes it, the white point or divisor), the motion vectors (size, sampling, scale), size changes,
resets. What the game does not hand over but something else covers says who covers it; with no
exposure from anywhere, a hint says to try Preprocess with Exposure = auto if the picture looks wrong.
Depth is shown only with Motion Without Depth = 0 (NVIDIA's DLL does not use depth). The name at the
start of each line has a colour: green = from the game, yellow = not from the game but filled in (what
fills it in is on the line), orange = not from the game and nothing fills it in.

Preprocess (optional, off by default): [Preprocess] in dlssnr-amd.ini in the game folder (written
at install; the ReShade routes also show it on the Add-ons page). It changes the picture the
network is shown (exposure, display curve, contrast, saturation), and so how NR edits the picture.
Two uses: a personal look in any game (it departs from the original look; the result may be better
or worse), and games that do not hand their exposure to the upscaler, which it fixes (see 007 First
Light below). Ctrl+F10 switches it for the current run, to compare. The file explains every
setting.

RenoDX style: the processing around NR (how the picture is handed to NR and how NR's result is put
back into the picture) follows RenoDX's DLSS5 add-on v8.5.0-rc10, with RenoDX's own shaders (MIT,
licence in dlssnr-amd/shaders/renodx/RenoDX-LICENSE.txt). The NR network itself is the same.
  - optiscaler route: the default processing (the installer can choose the wilsjo2 style instead,
    --wilsjo2); every layer of the NR chain can choose it. Its settings are on the layer's page in NR
    Control. A dlssnr-amd.ini without [LayerN] sections (as the installer writes it) has one layer,
    with the [RenoDX] settings (Enabled = 1: the RenoDX style; 0: the wilsjo2 style); the first change
    of the chain in NR Control turns it into a [Layer1] section.
  - reshade-feeder routes: the default processing too (the installer can choose the wilsjo2 style,
    --wilsjo2). Choosing RenoDX or wilsjo2 for a layer on the Add-ons page applies at once; without
    [LayerN] sections the switch is [RenoDX] Enabled in dlssnr-amd.ini (applied after a game restart),
    and with it on the layer's settings are in [RenoDX] and the NR settings in [DlssNr] have no effect.
  - Apart from Enabled, changes apply in the running game. The file explains every setting, and so does
    (?) in the menus. The settings keep RenoDX's own names, defaults and ranges.
  - NR on/off (NeuralUplift; F6 in game switches it for the current run, all layers together), the
    network's settings (Intensity, Style, Local tone, Local structure, Skin structure, Character mask),
    the network's resolution (NRResolutionScale). On the optiscaler route NR's place is the layer's
    Placement (formerly NRHookPoint), by default Present (the finished picture the game hands to the
    system).
  - Passes (NRPasses 1-4): each pass hands the previous pass's result to the network again. Passes
    2, 3 and 4 each have their own strengths (Transfer / Color) and network settings; with "Same as
    pass 1" on, the network settings follow pass 1, Intensity is always the pass's own.
    NRUnlockPasses = 1 allows up to 30 passes (as OptiScaler's UnlockPasses; RenoDX itself stops at
    4); passes 5 and later take pass 4's settings. Every pass runs the network once more, which
    lowers the frame rate a lot.
  - Adjust NR's look (NRLookMode and NRLook*): adjusts how much NR changed, directly on its result -
    brighten, darken, colour, hue, shadows / midtones / highlights, broad lighting and detail, dark
    halos, temporal smoothing. Off by default; off leaves the picture unchanged.
  - Screenshot (F5, also a button in NR Control and on the Add-ons page): the same frame without
    and with NR, saved to NR Screenshots/ in the game folder. By default the final picture: on the
    reshade-feeder routes the picture sent to the display; on the optiscaler route NR moves to the
    finished picture for one frame and moves back afterwards (when every layer is a RenoDX one). HDR
    pictures are saved as 16-bit PQ PNG (with cICP and ICC, so they stay HDR), SDR as 8-bit sRGB.
    JPEG, both PNG and JPEG, or an extra side-by-side picture can be chosen; NRScreenshotSource = 2
    saves the model's input and output (for diagnosis, without the game's tone mapping and UI).
    Nothing is saved while NR is off.
  - The automatic mask (NRAutoMask) is off by default, as in RenoDX (the wilsjo2 style's Character
    mask is on by default); when it is on, skin follows only Skin structure by default
    (NRSkinIndependent, as RenoDX).
  - Not taken over from RenoDX: Preset, the UI correction (our network has neither step), the frame
    generation settings, RenoDX's own diagnostics and internal debug items.
  - Preprocess can be used with it (reshade-feeder routes).

wilsjo2 style: the processing around NR of OptiScaler-NR (wilsjo2's OptiScaler fork, which the
optiscaler route used up to 0.0.7), ported to this project (after its method, none of its code). The
settings keep its [DlssNr] names, defaults and ranges: white point and exposure (the game's exposure,
manual, automatic metering), the HDR curve (ReversibleMode), detail and colour strength, the brightness
guard, skin and environment protection, passes and each pass's own settings, the model resolution
(WorkingScale, 0.25..2, above 1 is supersampling, ScalingDownscaler chooses the filter) and the
enlargement (Transfer), spatial compression (Spatial*).
  - optiscaler route: on the layer's page in NR Control (without [LayerN] sections, the [wilsjo2]
    section of dlssnr-amd.ini). On this project's test pictures its output was compared with
    OptiScaler-NR v0.8.91's, case by case; the differences are within the last rounding step.
  - reshade-feeder routes: these routes' original settings ([DlssNr], or wilsjo2 layers of the NR
    chain), the same processing with the intermediate results kept at 32-bit precision. Below 100%
    Model resolution the picture is made smaller these routes' own way, and Edge-aware lighting +
    colour and Classic are two more ways to enlarge; these routes get the game's finished picture,
    already exposed and tone mapped, so the white point is manual only (WhitePoint) and there are no
    exposure settings.

NR Control's place column shows where the layer actually runs: when a frame does not allow a layer placed
before the upscaler there (for example a wilsjo2 layer with an input it does not support), it runs after it,
shown as "After the upscaler (for now)" (orange, explained on hover). The settings file keeps "before", and the
layer goes back when the frame allows it.

Logs: dlssnr-amd.log (and OptiScaler.log or ReShade.log), all in the game folder.

Known issues
------------
- 32-bit games at 4K: on the first launch the 32-bit address space is tight (ReShade compiles its
  shaders at the same time). If dlssnr-amd.log shows out of host memory, NR retries every few
  seconds by itself; if it never succeeds, please send us the log.
- 007 First Light (optiscaler route): with NR before or after the upscaler the picture turns
  green and grainy, and textures look wrong. The game hands the upscaler a picture that is not yet
  tone mapped or graded, and no exposure. Turn on Preprocess (above) with Exposure = auto. The older
  workarounds (Paper white lowered to about 0.06, or [DlssNr] FinishedPicture=true) are no longer
  recommended.

Uninstall
---------
    bash install.sh "/path/to/<game folder>" remove

Only the files in the installation record are deleted (dlssnr-amd.ini and the logs included). A file
of the game folder that the installation overwrote (for example another mod's dxgi.dll) is not
brought back.
