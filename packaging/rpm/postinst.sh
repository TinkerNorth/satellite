#!/bin/sh
# RPM %post scriptlet: runs after files are unpacked into the filesystem.
#
# Mirrors packaging/debian/postinst: reload udev so the bundled
# 70-satellite-uinput.rules takes effect, load the uinput module so the
# device node exists in this session, and best-effort-add SUDO_USER to
# the `input` group. RPM passes 1=install, 2=upgrade rather than dpkg's
# state argument, but we want the same behavior either way.
set -e

if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
    udevadm trigger --subsystem-match=misc --action=change || true
fi

if command -v modprobe >/dev/null 2>&1; then
    modprobe uinput || true
fi

# When invoked under `sudo dnf install ...`, SUDO_USER is the calling
# user. Add them to the input group so they get /dev/uinput access.
# Skipped on dnf upgrade (no SUDO_USER) and on package transactions
# the user didn't initiate directly (e.g. unattended-upgrade equivalent).
if [ -n "${SUDO_USER:-}" ] && [ "${SUDO_USER}" != "root" ]; then
    if getent group input >/dev/null 2>&1; then
        if ! id -nG "${SUDO_USER}" | tr ' ' '\n' | grep -qx input; then
            usermod -aG input "${SUDO_USER}" || true
            echo ""
            echo "Satellite: added ${SUDO_USER} to the 'input' group."
            echo "           Log out and back in for the change to take effect."
            echo ""
        fi
    fi
else
    cat <<'EOF'

Satellite installed. To grant your user access to /dev/uinput:

    sudo usermod -aG input "$USER"
    # then log out and back in, or run `newgrp input` for the current shell.

EOF
fi

exit 0
