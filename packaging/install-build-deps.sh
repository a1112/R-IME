#!/bin/sh
# Run as root inside a Debian 13 / trixie build environment.
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
 build-essential cmake ninja-build pkg-config git ca-certificates python3 \
 nlohmann-json3-dev libyaml-cpp-dev librime-dev librime-bin \
 rime-prelude rime-essay rime-data-luna-pinyin rime-data-double-pinyin \
 libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev fcitx5-modules-dev \
 libcairo2-dev libpango1.0-dev libwayland-dev wayland-protocols \
 libxcb1-dev libxcb-randr0-dev libxcb-shape0-dev libxkbcommon-dev \
 qt6-base-dev qt6-declarative-dev qt6-wayland \
 qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
 qml6-module-qtquick-window qml6-module-qtqml-workerscript \
 qml6-module-qtquick-dialogs qml6-module-qtquick-templates \
 fonts-noto-cjk xvfb xauth
