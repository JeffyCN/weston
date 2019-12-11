#!/bin/bash

set -o xtrace -o errexit

echo 'deb http://deb.debian.org/debian buster-backports main' >> /etc/apt/sources.list
apt-get update
apt-get -y --no-install-recommends install \
	autoconf \
	automake \
	build-essential \
	curl \
	doxygen \
	freerdp2-dev \
	git \
	libcairo2-dev \
	libcolord-dev \
	libdbus-1-dev \
	libegl1-mesa-dev \
	libevdev-dev \
	libexpat1-dev \
	libffi-dev \
	libgbm-dev \
	libgdk-pixbuf2.0-dev \
	libgles2-mesa-dev \
	libglu1-mesa-dev \
	libgstreamer1.0-dev \
	libgstreamer-plugins-base1.0-dev \
	libinput-dev \
	libjpeg-dev \
	libjpeg-dev \
	liblcms2-dev \
	libmtdev-dev \
	libpam0g-dev \
	libpango1.0-dev \
	libpipewire-0.2-dev \
	libpixman-1-dev \
	libpng-dev \
	libsystemd-dev \
	libtool \
	libudev-dev \
	libva-dev \
	libvpx-dev \
	libwayland-dev \
	libwebp-dev \
	libx11-dev \
	libx11-xcb-dev \
	libxcb1-dev \
	libxcb-composite0-dev \
	libxcb-xfixes0-dev \
	libxcb-xkb-dev \
	libxcursor-dev \
	libxkbcommon-dev \
	libxml2-dev \
	mesa-common-dev \
	ninja-build \
	pkg-config \
	python3-pip \
	python3-setuptools \
	xwayland \


pip3 install --user git+https://github.com/mesonbuild/meson.git@0.49
# for documentation
pip3 install sphinx==2.1.0 --user
pip3 install breathe==4.13.0.post0 --user
pip3 install sphinx_rtd_theme==0.4.3 --user

git clone --branch 1.17.0 --depth=1 https://gitlab.freedesktop.org/wayland/wayland
export MAKEFLAGS="-j4"
cd wayland
git show -s HEAD
mkdir build
cd build
../autogen.sh --disable-documentation
make install
cd ../../

mkdir -p /tmp/.X11-unix
chmod 777 /tmp/.X11-unix
