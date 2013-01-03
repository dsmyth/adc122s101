BB=$PWD/../Beaglebone-Buildroot
KERNEL=linux-HEAD
export KERNELDIR=${BB}/output/build/${KERNEL}

export PATH=${PATH}:${BB}/output/host/usr/bin

unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS

export ARCH="arm"
export CROSS_COMPILE="arm-linux-gnueabihf-"
export CC="${CROSS_COMPILE}gcc"
export LD="${CROSS_COMPILE}ld"
export STRIP="${CROSS_COMPILE}strip"
make $*
