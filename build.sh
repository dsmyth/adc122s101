BB=$HOME/Work/Beaglebone-Buildroot
KERNEL=linux-master
export KERNELDIR=${BB}/output/build/${KERNEL}

export PATH=${PATH}:${BB}/output/host/opt/ext-toolchain/bin:${BB}/output/host/usr/bin

unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS

export ARCH="arm"
export CROSS_COMPILE="arm-linux-gnueabihf-"
export CC="${CROSS_COMPILE}gcc"
export CXX="${CROSS_COMPILE}g++"
export LD="${CROSS_COMPILE}ld"
export STRIP="${CROSS_COMPILE}strip"
make $* BINDIR=/export/rootfs/root MODDIR=/export/rootfs/lib/modules
