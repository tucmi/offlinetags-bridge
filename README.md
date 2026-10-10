# offlinetags-bridge

Brings [OfflineTags](https://github.com/fiker31/Offline-Tags) privacy-badge detection into photo
editors. A small local daemon keeps the models loaded, and a thin client hands it one photo at a
time. Two integrations exist:

- **IrfanView:** press `Shift+1` on an open photo and get the processed copy back.
- **Affinity** (and other hosts of Photoshop `.8bf` filters): an **Analyze** filter that finds the
  badges on the current layer and applies what each one asks for.

The models run on ONNX Runtime, so PyTorch is not needed at runtime.

**Status: prototype.** Windows only so far. Confirmed with IrfanView 4.76 and Affinity 3.3.

## What it does

OfflineTags badges are small coloured circles people wear to state how they want to appear in photos.

| Badge | Result |
| --- | --- |
| **Blur Me** (blue) | the person's face is pixelated |
| **Tag Me** (green), **Upload Me** (yellow) | a consent box is drawn around the person |
| **No Photo** (red) | nothing is written; you get a warning not to share the photo |
| none | nothing is written |

The original is never touched unless you ask for it: the result is saved next to it as
`photo.offlinetags.jpg`.

## Setup

Requires Python 3.10 or newer.

```powershell
git clone <this repo>
cd offlinetags-bridge
python -m venv .venv
.venv\Scripts\pip install -e .
```

### Models

The two ONNX models are not in the repository. Create them once from the upstream weights. This
step needs PyTorch and ultralytics, so use a separate, throwaway environment:

```powershell
python -m venv .venv-export
.venv-export\Scripts\pip install torch --index-url https://download.pytorch.org/whl/cpu
.venv-export\Scripts\pip install -r tools\requirements-export.txt
.venv-export\Scripts\python tools\export_onnx.py
```

This writes `models\tags.onnx` (badge detector, taken from the `offlinetags` 1.0.2 wheel on PyPI)
and `models\person.onnx` (stock YOLOv8n), about 25 MB together. Afterwards `.venv-export` can be
deleted.

Models are looked up in `OFFLINETAGS_BRIDGE_MODELS`, then `models\` in the checkout, then
`~\.offlinetags-bridge\models`.

> Exporting loads `.pt` files, which are Python pickles and can run code. Only export weights you trust.

## Command line

```powershell
offlinetags-bridge process photo.jpg              # writes photo.offlinetags.jpg
offlinetags-bridge process photo.jpg -o out.png   # choose the output
offlinetags-bridge process --in-place photo.jpg   # overwrite the original
offlinetags-bridge process --no-annotate *.jpg    # only blur, no consent boxes
offlinetags-bridge process --json photo.jpg       # detections and blurred regions as JSON
offlinetags-bridge status | start | stop
```

The first call starts the daemon in the background (about 2.5 s). It stops by itself after
30 minutes without requests.

Exit codes: `0` done, `1` error, `2` at least one photo carries a **No Photo** badge.

## IrfanView

`offlinetags-irfanview.exe` (in `.venv\Scripts`) is a windowless variant of the client made for
IrfanView's external editor feature. It takes the file name IrfanView passes, opens the processed
copy in a new IrfanView window, and shows a message box for **No Photo**, for photos without
badges, and for errors.

Register it by hand:

1. In IrfanView open **Options > Properties/Settings > Miscellaneous**.
2. Under **Set external editors**, put the full path of `offlinetags-irfanview.exe` into a free slot.
3. Open a photo and press `Shift+1` (or `Shift+2`, `Shift+3` for the other slots).

Or with the script, while IrfanView is closed:

```powershell
.\irfanview\install.ps1              # slot 1; -Slot 2 or -Slot 3 for another
.\irfanview\install.ps1 -Uninstall
```

If PowerShell refuses to run scripts, use
`powershell -ExecutionPolicy Bypass -File irfanview\install.ps1`.

The script writes `ExternalViewer` into the `[Others]` section of `%APPDATA%\IrfanView\i_view64.ini`
and saves a backup first.

For folders, use the command line or IrfanView's thumbnail view with "Send all filenames in one
call" enabled; the client accepts several files.

## Affinity

`OfflineTags.8bf` is a Photoshop-compatible filter plug-in with one menu entry, **Analyze**. It
looks for badges on the current pixel layer, applies the matching action from the table above as
one undoable step, and then reports what it found and did:

| Detected | What Analyze does |
| --- | --- |
| **Blur Me** | pixelates the wearer's face |
| **Tag Me**, **Upload Me** | draws a consent box around the wearer |
| **No Photo** (alone or with others) | changes nothing and warns not to share the photo |
| no badge | changes nothing |

Build and install (needs Visual Studio or the Build Tools with the C++ workload; the Adobe SDK is
not required):

```powershell
.\affinity\build.ps1
.\affinity\install.ps1
```

`install.ps1` copies the plug-in to `~\.offlinetags-bridge\plugins` and records the path of
`offlinetags-bridge.exe` in `~\.offlinetags-bridge\client.txt`, which is how the plug-in finds the
client. Then, once, in Affinity:

1. Open **Settings > Photoshop Plugins**.
2. Add `~\.offlinetags-bridge\plugins` as a plug-in search folder (the script prints the full path).
3. Tick **Allow unknown plugins to be used**.
4. Restart Affinity.

With a document open and a pixel layer selected, the filter is at
**Pixel > Filters > Plugins > OfflineTags > Analyze**.

It works on 8-bit and 16-bit RGB documents. Detection sees only the layer (or the bounds of the
selection) the filter runs on, so apply it to the photo layer without a selection. The plug-in
passes the pixels through an uncompressed temporary file in `~\.offlinetags-bridge\tmp`, which it
deletes afterwards.

If the filter does nothing, look at `~\.offlinetags-bridge\plugin.log`: the plug-in records every
call from the host there. To rebuild, close Affinity first, because it keeps the `.8bf` open.

## How it works

```text
IrfanView --Shift+1--> offlinetags-irfanview.exe --HTTP on 127.0.0.1--> daemon (models loaded)
                              |                                              |
                              +--- opens photo.offlinetags.jpg <--- writes --+
```

- **Daemon** ([daemon.py](src/offlinetags_bridge/daemon.py)): standard-library HTTP server bound to
  `127.0.0.1` on a free port. It writes the port and a random token to
  `~\.offlinetags-bridge\daemon.json`; requests without the token are refused, so web pages and
  other programs cannot make it read or write files. Log: `~\.offlinetags-bridge\daemon.log`.
- **Client** ([client.py](src/offlinetags_bridge/client.py)): standard library only, so it starts
  instantly. Sends file paths, not pixels.
- **Pipeline** ([pipeline.py](src/offlinetags_bridge/pipeline.py)): a port of the upstream logic.
  Badge detection and person detection run on ONNX Runtime ([yolo.py](src/offlinetags_bridge/yolo.py));
  faces are found with OpenCV Haar cascades, as upstream does.

- **Affinity plug-in** ([plugin.c](affinity/plugin.c)): a small C DLL that writes the layer to a
  BMP, runs the client on it and copies the changed pixels back. The host interface is declared in
  [filter_api.h](affinity/filter_api.h) and the menu entry in [pipl.rc](affinity/pipl.rc).

`POST /process` takes `{"input", "output", "annotate", "jpeg_quality"}` and returns the action, the
detections and `blur_regions` (pixel boxes of every blurred face), which a future editor plugin can
use to blur on its own layer.

## Compared with upstream

Measured on 8 of the upstream sample photos (17 to 22 megapixels) on a laptop CPU:

- **Detections:** identical to the PyTorch models for both badge and person detection, same boxes
  and same confidences to three decimals.
- **Speed:** 0.3 to 2.4 s per photo with the daemon running. Most of that is decoding, face search
  and re-encoding at this resolution.
- **Size:** about 210 MB of installed dependencies plus 25 MB of models. The export environment
  with CPU-only PyTorch and ultralytics is 1.1 GB, and upstream additionally needs facenet-pytorch.
  OpenCV is the largest remaining part (110 MB).

Deliberate differences:

- The mosaic gets coarser on large faces (at most 12 blocks across). Upstream's fixed 25-pixel
  blocks leave a face in a high-resolution photo recognisable.
- Consent boxes use the intended colours and scale with the image. Upstream passes RGB values to
  OpenCV, which made the yellow **Upload Me** box blue.
- Smart batch (matching faces across photos) is not ported.

## Limitations

- The badge model is upstream's and is trained on its four badge designs. One of the sample photos
  yields a low-confidence false **Blur Me** on an **Upload Me** badge, upstream as well.
- Haar cascades only find frontal faces reliably. On profile faces the blur can sit off-centre, and
  a second face in the search area can be blurred too.
- Output files carry no EXIF data. Orientation is applied to the pixels.
- OpenCV is pinned below 5 because version 5 removed the Haar cascade classifier.

## Development

```powershell
.venv\Scripts\pip install -e ".[dev]"
.venv\Scripts\pytest
```

The core tests need no model files. The plug-in tests run once `affinity\build.ps1` has been run:
they load the `.8bf` into a minimal filter host ([filter_host.py](tests/filter_host.py)) and, with
the models and `samples/` present, run it end to end.

## Licence

MIT, see [LICENSE](LICENSE). The pipeline is ported from `offlinetags` 1.0.2 (MIT). The exported
models are not redistributed here: YOLOv8 weights and models trained with ultralytics fall under
Ultralytics' AGPL-3.0 terms, so check those before publishing model files.
