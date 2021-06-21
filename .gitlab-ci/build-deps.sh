#!/bin/bash
# 
# Builds the dependencies required for any OS/architecture combination. See
# .gitlab-ci.yml for more information. This script is called from an
# OS-specific build scripts like debian-install.sh.

set -o xtrace -o errexit

# Set concurrency to an appropriate level for our shared runners, falling back
# to the conservative default form before we had this variable.
export MAKEFLAGS="-j${FDO_CI_CONCURRENT:-4}"
export NINJAFLAGS="-j${FDO_CI_CONCURRENT:-4}"

# Build and install Meson. Generally we want to keep this in sync with what
# we require inside meson.build, however we require at least 0.57.0 per
# wayland/weston@bcf37c937a36, and 0.57.1 to fix the TAP parser when a test
# exits unsuccessfully
pip3 install --user git+https://github.com/mesonbuild/meson.git@0.57.1
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
git clone --depth=1 --branch=drm-next-2020-06-11-1 https://anongit.freedesktop.org/git/drm/drm.git linux
cd linux
if [[ "${BUILD_ARCH}" = "x86-64" ]]; then
	LINUX_ARCH=x86
fi
if [[ -z "${KERNEL_DEFCONFIG}" ]]; then
	echo "Invalid or missing \$KERNEL_DEFCONFIG"
	exit
fi
if [[ -z "${KERNEL_IMAGE}" ]]; then
	echo "Invalid or missing \$KERNEL_IMAGE"
	exit
fi
make ARCH=${LINUX_ARCH} ${KERNEL_DEFCONFIG}
make ARCH=${LINUX_ARCH} kvmconfig
./scripts/config --enable CONFIG_DRM_VKMS
make ARCH=${LINUX_ARCH} oldconfig
make ARCH=${LINUX_ARCH}
cd ..
mkdir /weston-virtme
mv linux/arch/${LINUX_ARCH}/boot/${KERNEL_IMAGE} /weston-virtme/
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
meson build -Ddocumentation=false
ninja ${NINJAFLAGS} -C build install
cd ..

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
git clone --single-branch --branch 0.3.31 https://gitlab.freedesktop.org/pipewire/pipewire.git pipewire
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
