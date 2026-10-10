"""Tests for affinity/build/OfflineTags.8bf. Skipped until the plug-in is built.

The end-to-end tests also need the ONNX models and the photos in samples/.
"""

import ctypes
import os
import struct
import sys
from pathlib import Path

import numpy as np
import pytest

from filter_host import NO_ERR, USER_CANCELED, FilterRecord, run_filter

ROOT = Path(__file__).resolve().parents[1]
# the override lets a fresh build be tested while a host still has the regular file open
PLUGIN = Path(os.environ.get("OFFLINETAGS_BRIDGE_PLUGIN") or ROOT / "affinity" / "build" / "OfflineTags.8bf")
CLIENT = Path(sys.executable).with_name("offlinetags-bridge.exe")
SAMPLES = ROOT / "samples"

pytestmark = pytest.mark.skipif(
    sys.platform != "win32" or not PLUGIN.exists(), reason="plug-in not built (affinity/build.ps1)")


def test_filter_record_layout():
    # the same numbers are static asserts in affinity/filter_api.h
    assert ctypes.sizeof(FilterRecord) == 556
    assert FilterRecord.inData.offset == 88
    assert FilterRecord.depth.offset == 480


def _pipl_properties() -> dict[str, bytes]:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.LoadLibraryExW.restype = ctypes.c_void_p
    kernel32.FindResourceW.restype = ctypes.c_void_p
    kernel32.FindResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p]
    kernel32.LoadResource.restype = ctypes.c_void_p
    kernel32.LoadResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel32.LockResource.restype = ctypes.c_void_p
    kernel32.LockResource.argtypes = [ctypes.c_void_p]
    kernel32.SizeofResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    module = kernel32.LoadLibraryExW(str(PLUGIN), None, 0x2)  # LOAD_LIBRARY_AS_DATAFILE
    resource = kernel32.FindResourceW(module, 1, "PiPL")
    assert resource, "no PiPL resource"
    data = ctypes.string_at(kernel32.LockResource(kernel32.LoadResource(module, resource)),
                            kernel32.SizeofResource(module, resource))

    signature, version, count = struct.unpack_from("<hii", data, 0)
    assert (signature, version) == (1, 0)
    properties, offset = {}, 10
    for _ in range(count):
        vendor, key, _id, length = struct.unpack_from("<4s4sii", data, offset)
        assert vendor[::-1] == b"8BIM"
        properties[key[::-1].decode()] = data[offset + 16:offset + 16 + length]
        offset += 16 + ((length + 3) & ~3)
    assert offset == len(data), "property lengths do not add up"
    return properties


def _pascal(data: bytes) -> str:
    return data[1:1 + data[0]].decode()


def test_pipl_describes_the_filter():
    properties = _pipl_properties()
    assert properties["kind"][::-1] == b"8BFM"
    assert _pascal(properties["name"]) == "Analyze"
    assert _pascal(properties["catg"]) == "OfflineTags"
    assert struct.unpack("<i", properties["vers"])[0] == 4 << 16
    assert properties["mode"][0] & 0x10, "RGB mode flag"
    assert properties["fici"] == bytes([1, 1, 0, 0]) * 7
    assert properties["enbl"].rstrip(b"\0") == b"in (PSHOP_ImageMode, RGBMode, RGB48Mode)"

    entry_point = properties["8664"].split(b"\0")[0].decode()
    assert hasattr(ctypes.CDLL(str(PLUGIN)), entry_point)


def _sample(name: str) -> np.ndarray:
    """A sample photo as RGB, halved to keep the test quick."""
    from offlinetags_bridge.pipeline import read_image

    path = SAMPLES / name
    if not path.exists() or not CLIENT.exists():
        pytest.skip("needs samples/ and an installed client")
    return np.ascontiguousarray(read_image(path)[::2, ::2, ::-1])


@pytest.fixture
def plugin_env(monkeypatch):
    from offlinetags_bridge.paths import TAG_MODEL, models_dir

    if not (models_dir() / TAG_MODEL).exists():
        pytest.skip("needs the ONNX models")
    monkeypatch.setenv("OFFLINETAGS_BRIDGE_CLIENT", str(CLIENT))
    monkeypatch.setenv("OFFLINETAGS_BRIDGE_QUIET", "1")


def _changed_box(before: np.ndarray, after: np.ndarray):
    rows, cols = np.nonzero((before != after).any(axis=2))
    return rows.min(), rows.max(), cols.min(), cols.max()


@pytest.mark.parametrize("advance_state", [False, True], ids=["classic", "advanceState"])
def test_blur_me_changes_only_the_face(plugin_env, advance_state):
    rgb = _sample("blur-me.jpg")
    result, out = run_filter(str(PLUGIN), rgb, advance_state)
    assert result == NO_ERR
    top, bottom, left, right = _changed_box(rgb, out)
    h, w = rgb.shape[:2]
    # the face sits in the upper middle of this portrait
    assert bottom < h * 0.6 and left > w * 0.1 and right < w * 0.9
    assert (bottom - top) * (right - left) > 0.02 * h * w


def test_16_bit_keeps_untouched_pixels_exact(plugin_env):
    rgb8 = _sample("blur-me.jpg")
    rgb16 = (rgb8.astype(np.uint32) * 32768 // 255).astype(np.uint16) + (rgb8 % 3 == 0)  # not 8-bit clean
    rgb16 = np.minimum(rgb16, 32768).astype(np.uint16)
    result, out = run_filter(str(PLUGIN), rgb16)
    assert result == NO_ERR
    top, bottom, left, right = _changed_box(rgb16, out)
    assert bottom < rgb16.shape[0] * 0.6
    assert out.max() <= 32768


def test_tag_me_gets_a_consent_box(plugin_env):
    rgb = _sample("tag-me.jpg")
    result, out = run_filter(str(PLUGIN), rgb, advance_state=True)
    assert result == NO_ERR
    changed = (rgb != out).any(axis=2)
    top, bottom, left, right = _changed_box(rgb, out)
    # a box outline around the person: tall and wide, but mostly untouched inside
    assert (bottom - top) > 0.5 * rgb.shape[0] and (right - left) > 0.3 * rgb.shape[1]
    assert changed[top:bottom, left:right].mean() < 0.2


def test_messages_say_what_was_found_and_done(plugin_env):
    from offlinetags_bridge.paths import state_dir

    log = state_dir() / "plugin.log"

    def last_message(name):
        run_filter(str(PLUGIN), _sample(name), advance_state=True)
        lines = log.read_text(encoding="utf-8", errors="replace").split("message: ")
        return lines[-1]

    both = last_message("blur-and-upload.jpg")
    assert "1 x Blur Me" in both and "1 x Upload Me" in both
    assert "pixelated" in both and "consent box" in both
    assert "do not share" in last_message("no-photo.jpg")
    assert "no privacy tags" in last_message("no-badge.jpg")


@pytest.mark.parametrize("advance_state", [False, True], ids=["classic", "advanceState"])
@pytest.mark.parametrize("name", ["no-photo.jpg", "no-badge.jpg"])
def test_other_outcomes_leave_the_image_alone(plugin_env, name, advance_state):
    rgb = _sample(name)
    result, out = run_filter(str(PLUGIN), rgb, advance_state)
    assert result == USER_CANCELED
    assert np.array_equal(out, rgb)
