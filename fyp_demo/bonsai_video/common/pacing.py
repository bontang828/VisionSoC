"""Global pacing: every scene plays SLOWDOWN x slower so on-screen text can
be read at voice-over speed. Imported for its side effect from common.palette
(which every scene imports), so no scene needs to opt in.

A scene can override the factor with a class attribute, e.g.:
    SLOWDOWN = 1.0   # kernel demo / outro: real footage or fixed length
    SLOWDOWN = 1.5   # contribution cards: lots of words on screen

Only play() calls that pass an explicit run_time and wait() durations are
scaled; animations that carry their own internal run_time keep it (their
defaults are already leisurely).
"""
from manim import Scene

FACTOR = 1.2

_orig_play = Scene.play
_orig_wait = Scene.wait


def _factor(scene):
    return getattr(scene, "SLOWDOWN", FACTOR)


def _play(self, *args, **kwargs):
    if kwargs.get("run_time") is not None:
        kwargs["run_time"] = kwargs["run_time"] * _factor(self)
    return _orig_play(self, *args, **kwargs)


def _wait(self, duration=1.0, **kwargs):
    return _orig_wait(self, duration * _factor(self), **kwargs)


Scene.play = _play
Scene.wait = _wait
