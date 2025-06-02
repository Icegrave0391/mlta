# Configurations

KERNEL_SRC="$(pwd)/../kernels/linux-noftrace"
IRDUMPER="$(pwd)/IRDumper/build/lib/libDumper.so"
CC="$(pwd)/llvm-project/prefix/bin/clang"
LD="$(pwd)/llvm-project/prefix/bin/ld.lld"
AR="$(pwd)/llvm-project/prefix/bin/llvm-ar"
AS="$(pwd)/llvm-project/prefix/bin/llvm-as"
NM="$(pwd)/llvm-project/prefix/bin/llvm-nm"
STRIP="$(pwd)/llvm-project/prefix/bin/llvm-strip"
OBJCOPY="$(pwd)/llvm-project/prefix/bin/llvm-objcopy"
OBJDUMP="$(pwd)/llvm-project/prefix/bin/llvm-objdump"
READELF="$(pwd)/llvm-project/prefix/bin/llvm-readelf"
# CONFIG="defconfig"
CONFIG="olddefconfig"
#CONFIG="allyesconfig"

# Use -Wno-error to avoid turning warnings into errors
NEW_CMD="\n\n\
KBUILD_USERCFLAGS += -Wno-error -g -Xclang -no-opaque-pointers -Xclang -flegacy-pass-manager -Xclang -load -Xclang $IRDUMPER\nKBUILD_CFLAGS += -Wno-error -g -Xclang -no-opaque-pointers -Xclang -flegacy-pass-manager -Xclang -load -Xclang $IRDUMPER"

# Back up Linux Makefile
#cp $KERNEL_SRC/Makefile $KERNEL_SRC/Makefile.bak

if [ ! -f "$KERNEL_SRC/Makefile.bak" ]; then
	echo "Back up Linux Makefile first"
	exit -1
fi

# The new flags better follow "# Add user supplied CPPFLAGS, AFLAGS and CFLAGS as the last assignments"
echo -e $NEW_CMD >$KERNEL_SRC/IRDumper.cmd
cat $KERNEL_SRC/Makefile.bak $KERNEL_SRC/IRDumper.cmd >$KERNEL_SRC/Makefile

cd $KERNEL_SRC && make CC=$CC LD=$LD AR=$AR AS=$AS NM=$NM STRIP=$STRIP OBJCOPY=$OBJCOPY OBJDUMP=$OBJDUMP READELF=$READELF $CONFIG
echo $CLANG
echo $NEW_CMD
make CC=$CC LD=$LD AR=$AR AS=$AS NM=$NM STRIP=$STRIP OBJCOPY=$OBJCOPY OBJDUMP=$OBJDUMP READELF=$READELF -j`nproc` -k -i

