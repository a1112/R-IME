# Fcitx Wayland ABI bridge

These unmodified generated wrappers come from fcitx/fcitx5 tag 5.1.12,
commit 7d71627695b49c06a4270f4d8106e84afb75cddb, src/lib/fcitx-wayland.
The unchanged waylandim_public.h comes from src/frontend/waylandim at the
same revision; Debian's modules development package omits this interface.
Copyright Fcitx contributors; LGPL-2.1-or-later, license included.

WlSurface is necessary because the host classic UI interprets shared Wayland
surface userdata as this wrapper. Its application userData stays null, so the
host does not interpret R-IME surfaces as classic UI windows. The v2 input-method
header bridges an internal Fcitx wrapper returned by its public addon API.
This bridge must be compiled with hidden visibility and tested when Fcitx updates.
The protocol XML retains its own upstream license header.
