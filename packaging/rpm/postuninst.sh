#!/bin/sh
# RPM %postun scriptlet — runs after files are removed from the filesystem.
#
# Mirrors packaging/debian/postrm: reload udev. We deliberately leave the
# `input` group membership in place since other tools (joystick devices,
# barcode scanners, accessibility software) rely on it.
set -e

if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
fi

exit 0
