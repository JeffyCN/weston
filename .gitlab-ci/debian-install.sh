#!/bin/bash
# 
# Constructs the base container image used to build Weston within CI. Per the
# comment at the top of .gitlab-ci.yml, any changes in this file must bump the
# $FDO_DISTRIBUTION_TAG variable so we know the container has to be rebuilt.

set -o xtrace -o errexit

# Set concurrency to an appropriate level for our shared runners, falling back
# to the conservative default from before we had this variable.
export MAKEFLAGS="-j${FDO_CI_CONCURRENT:-4}"
export NINJAFLAGS="-j${FDO_CI_CONCURRENT:-4}"

# These get temporary installed for building Linux and then force-removed.
LINUX_DEV_PKGS="
	bc
	bison
	flex
	libelf-dev
"

# These get temporary installed for building Mesa and then force-removed.
MESA_DEV_PKGS="
	bison
	flex
	gettext
	libwayland-egl-backend-dev
	libxrandr-dev
	llvm-8-dev
	python-mako
	python3-mako
"

# Needed for running the custom-built mesa
MESA_RUNTIME_PKGS="
	libllvm8
"

echo 'deb http://deb.debian.org/debian buster-backports main' >> /etc/apt/sources.list
apt-get update
apt-get -y --no-install-recommends install \
	autoconf \
	automake \
	build-essential \
	curl \
	doxygen \
	gcovr \
	git \
	lcov \
	libasound2-dev \
	libbluetooth-dev \
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
	libjack-jackd2-dev \
	libjpeg-dev \
	libjpeg-dev \
	liblcms2-dev \
	libmtdev-dev \
	libpam0g-dev \
	libpango1.0-dev \
	libpixman-1-dev \
	libpng-dev \
	libpulse-dev \
	libsbc-dev \
	libsystemd-dev \
	libtool \
	libudev-dev \
	libva-dev \
	libvpx-dev \
	libvulkan-dev \
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
	qemu-system \
	sysvinit-core \
	xwayland \
	$MESA_RUNTIME_PKGS

apt-get -y --no-install-recommends -t buster-backports install \
	freerdp2-dev

# Build and install Meson. Generally we want to keep this in sync with what
# we require inside meson.build, however per wayland/weston@bcf37c937a36,
# we use a higher version here 
pip3 install --user git+https://github.com/mesonbuild/meson.git@0.57.0
export PATH=$HOME/.local/bin:$PATH

# Our docs are built using Sphinx (top-level organisation and final HTML/CSS
# generation), Doxygen (parse structures/functions/comments from source code),
# Breathe (a bridge between Doxygen and Sphinx), and we use the Read the Docs
# theme for the final presentation.
pip3 install sphinx==2.1.0 --user
pip3 install breathe==4.13.0.post0 --user
pip3 install sphinx_rtd_theme==0.4.3 --user

# Build a Linux kernel for use in testing. We enable the VKMS module so we can
# predictably test the DRM backend in the absence of real hardware. We lock the
# version here so we see predictable results.
apt-get -y --no-install-recommends install $LINUX_DEV_PKGS
git clone --depth=1 --branch=drm-next-2020-06-11-1 https://anongit.freedesktop.org/git/drm/drm.git linux
cd linux
make x86_64_defconfig
make kvmconfig
./scripts/config --enable CONFIG_DRM_VKMS
make oldconfig
make
cd ..
mkdir /weston-virtme
mv linux/arch/x86/boot/bzImage /weston-virtme/bzImage
mv linux/.config /weston-virtme/.config
rm -rf linux

# Build virtme, a QEMU wrapper: https://github.com/amluto/virtme
# 
# virtme makes our lives easier by abstracting handling of the console,
# filesystem, etc, so we can pretend that the VM we execute in is actually
# just a regular container.
#
# The reason why we are using a fork here is that it adds a patch to have the
# --script-dir command line option. With that we can run scripts that are in a
# certain folder when virtme starts, which is necessary in our use case.
#
# The upstream also has some commands that could help us to reach the same
# results: --script-sh and --script-exec. Unfornutately they are not completely
# implemented yet, so we had some trouble to use them and it was becoming
# hackery.
#
git clone https://github.com/ezequielgarcia/virtme
cd virtme
git checkout -b snapshot 69e3cb83b3405edc99fcf9611f50012a4f210f78
./setup.py install
cd ..

# Build and install Wayland; keep this version in sync with our dependency
# in meson.build.
git clone --branch 1.18.0 --depth=1 https://gitlab.freedesktop.org/wayland/wayland
cd wayland
git show -s HEAD
mkdir build
cd build
../autogen.sh --disable-documentation
make install
cd ../../

# Keep this version in sync with our dependency in meson.build. If you wish to
# raise a MR against custom protocol, please change this reference to clone
# your relevant tree, and make sure you bump $FDO_DISTRIBUTION_TAG.
git clone --branch 1.19 https://gitlab.freedesktop.org/wayland/wayland-protocols
cd wayland-protocols
git show -s HEAD
mkdir build
cd build
../autogen.sh
make install
cd ../../
rm -rf wayland-protocols

# Build and install our own version of Mesa. Debian provides a perfectly usable
# Mesa, however llvmpipe's rendering behaviour can change subtly over time.
# This doesn't work for our tests which expect pixel-precise reproduction, so
# we lock it to a set version for more predictability. If you need newer
# features from Mesa then bump this version and $FDO_DISTRIBUTION_TAG, however
# please be prepared for some of the tests to change output, which will need to
# be manually inspected for correctness.
apt-get -y --no-install-recommends install $MESA_DEV_PKGS
git clone --single-branch --branch 20.3 --shallow-since='2020-12-15' https://gitlab.freedesktop.org/mesa/mesa.git mesa
cd mesa
git checkout -b snapshot mesa-20.3.1
meson build -Dauto_features=disabled \
	-Dgallium-drivers=swrast -Dvulkan-drivers= -Ddri-drivers=
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf mesa

# PipeWire is used for remoting support. Unlike our other dependencies its
# behaviour will be stable, however as a pre-1.0 project its API is not yet
# stable, so again we lock it to a fixed version.
rm -rf pipewire
git clone --depth=1 --branch 0.3.31 https://gitlab.freedesktop.org/pipewire/pipewire.git pipewire
cd pipewire
meson build
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf pipewire

# seatd lets us avoid the pain of handling VTs manually through weston-launch
# or open-coding TTY assignment within Weston. We use this for our tests using
# the DRM backend.
git clone --depth=1 --branch 0.5.0 https://git.sr.ht/~kennylevinsen/seatd
cd seatd
meson build -Dauto_features=disabled \
	-Dseatd=enabled -Dlogind=enabled -Dserver=enabled \
	-Dexamples=disabled -Dman-pages=disabled
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf seatd

apt-get -y --autoremove purge $LINUX_DEV_PKGS
apt-get -y --autoremove purge $MESA_DEV_PKGS
