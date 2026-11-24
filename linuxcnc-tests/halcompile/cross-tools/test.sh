#!/bin/sh
set -eu

space_root=$(mktemp -d "$PWD/with space.XXXXXX")
trap 'rm -rf -- "$space_root"' EXIT
cp cross_tools.comp "$space_root/cross_tools.comp"
cat > "$space_root/cross_user.c" <<'EOF'
#include "hal.h"
int main(void) { return hal_init("cross_user") < 0; }
EOF

# Target tools that record their arguments and run the host tools. The
# compiler drops --sysroot so the host compiler keeps its own C library while
# LinuxCNC headers and libraries still have to be found in the test sysroot.
tools=$space_root/tools
mkdir -p "$tools"
for tool in gcc ld objdump; do
    cat > "$tools/target-$tool" <<EOF
#!/bin/sh
printf '\t' >> "\$0.log"
printf '%s\t' "\$@" >> "\$0.log"
printf '\n' >> "\$0.log"
for argument; do
    shift
    case \$argument in
        --sysroot=*) ;;
        *) set -- "\$@" "\$argument" ;;
    esac
done
exec $tool "\$@"
EOF
    chmod +x "$tools/target-$tool"
done
reset_tool_logs() {
    rm -f "$tools"/*.log
}
# Succeeds when one logged call of tool $1 received every following argument.
tool_called_with() {
    calls=$(cat "$tools/target-$1.log")
    shift
    for argument; do
        calls=$(printf '%s\n' "$calls" | grep -F -- "$(printf '\t%s\t' "$argument")") || return 1
    done
}

# An installed target LinuxCNC, as seen through the sysroot.
sysroot=$space_root/sysroot
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib"
ln -s "$EMC2_HOME/include" "$sysroot/usr/include/linuxcnc"
ln -s "$EMC2_HOME/lib/liblinuxcnchal.so" "$sysroot/usr/lib/liblinuxcnchal.so"
target_modinc=$space_root/target.modinc
sed -e 's|^RUN_IN_PLACE=.*|RUN_IN_PLACE=no|' \
    -e 's|^prefix := .*|prefix := /usr|' \
    -e 's|^RTLIBDIR := .*|RTLIBDIR := /usr/lib/linuxcnc/modules|' \
    "$(halcompile --print-modinc)" > "$target_modinc"

target_halcompile() {
    halcompile --modinc="$target_modinc" --cross-prefix="$tools/target-" \
        --sysroot="$sysroot" "$@"
}

(
    cd "$space_root"
    reset_tool_logs
    target_halcompile --compile cross_tools.comp
    test -s cross_tools.so
    tool_called_with gcc "--sysroot=$sysroot" -c cross_tools.c
    tool_called_with gcc "--sysroot=$sysroot" -shared
    tool_called_with ld -r
    tool_called_with objdump .rtapi_export

    reset_tool_logs
    target_halcompile --compile --userspace cross_user.c
    test -x cross_user
    tool_called_with gcc "--sysroot=$sysroot" "-L$sysroot/usr/lib" \
        -Wl,-rpath,/usr/lib
)

# Installation goes to the sysroot unless the caller stages into DESTDIR.
mkdir -p "$sysroot/usr/lib/linuxcnc/modules"
(
    cd "$space_root"
    target_halcompile --install cross_tools.comp
    target_halcompile --install --userspace cross_user.c
)
test -s "$sysroot/usr/lib/linuxcnc/modules/cross_tools.so"
test -x "$sysroot/usr/bin/cross_user"
rm "$sysroot/usr/lib/linuxcnc/modules/cross_tools.so" "$sysroot/usr/bin/cross_user"

stage=$space_root/stage
mkdir -p "$stage/usr/lib/linuxcnc/modules"
(
    cd "$space_root"
    DESTDIR=$stage target_halcompile --install cross_tools.comp
    DESTDIR=$stage target_halcompile --install --userspace cross_user.c
)
test -s "$stage/usr/lib/linuxcnc/modules/cross_tools.so"
test -x "$stage/usr/bin/cross_user"
test ! -e "$sysroot/usr/lib/linuxcnc/modules/cross_tools.so"
test ! -e "$sysroot/usr/bin/cross_user"

if halcompile --install --cross-prefix="$tools/target-" cross_tools.comp \
        >"$space_root/host-install-output" 2>&1; then
    echo "cross installation without a sysroot was accepted" >&2
    exit 1
fi
grep -F -- '--cross-prefix requires --sysroot' "$space_root/host-install-output"

if halcompile --compile --sysroot="$sysroot" cross_tools.comp \
        >"$space_root/rip-cross-output" 2>&1; then
    echo "cross compilation unexpectedly accepted a run-in-place Makefile.modinc" >&2
    exit 1
fi
grep -F 'requires an installed target Makefile.modinc' "$space_root/rip-cross-output"

# A native build ignores target tools and a sysroot exported by a cross SDK
# environment.
rm -f cross_tools.so
LD=false OBJDUMP=false SYSROOT="$sysroot" halcompile --compile cross_tools.comp
test -s cross_tools.so

# Compiler and make overrides may be commands with arguments.
(
    cd "$space_root"
    rm -f cross_tools.so
    halcompile --compile --cc="env gcc" --make="env make" cross_tools.comp
    test -s cross_tools.so
)

# Kbuild passes paths with spaces to the kernel build intact.
mkdir -p "$space_root/kernel dir" "$space_root/module dir"
cp "$(halcompile --print-modinc)" "$space_root/Makefile.modinc"
cat > "$space_root/kernel dir/Makefile" <<'EOF'
modules:
	@$(CC) --version > "$(CHECK_OUTPUT)"
	@printf '%s\n' '$(KBUILD_EXTRA_SYMBOLS)' '$(SUBDIRS)' >> "$(CHECK_OUTPUT)"
EOF
(
    cd "$space_root/module dir"
    make -s -f "$space_root/Makefile.modinc" modules \
        BUILDSYS=kbuild KERNELDIR="$space_root/kernel dir" \
        RTLIBDIR="$space_root/rt lib" CC="'$tools/target-gcc'" \
        CHECK_OUTPUT="$space_root/kbuild-output"
)
test -s "$space_root/kbuild-output"
grep -Fx -- "$space_root/rt lib/Module.symvers" "$space_root/kbuild-output"
grep -Fx -- "$space_root/module dir" "$space_root/kbuild-output"

# Kbuild rereads the component Makefile from inside the kernel tree, as
# scripts/Makefile.build does, so its Makefile.modinc include must resolve
# from there.
kbuild_root=$space_root/kbuild
mkdir -p "$kbuild_root/kernel"
sed -e 's|^BUILDSYS = .*|BUILDSYS = kbuild|' \
    -e "s|^KERNELDIR := .*|KERNELDIR := $kbuild_root/kernel|" \
    "$(halcompile --print-modinc)" > "$kbuild_root/Makefile.modinc"
cat > "$kbuild_root/kernel/Makefile" <<'EOF'
modules:
	@$(MAKE) -f "$(SUBDIRS)/Makefile" -f kbuild.mk component
EOF
cat > "$kbuild_root/kernel/kbuild.mk" <<'EOF'
component:
	@test "$(obj-m)" = cross_tools.o
	@touch "$(SUBDIRS)/cross_tools.ko"
EOF
(
    cd "$kbuild_root"
    cp ../cross_tools.comp .
    halcompile --compile --modinc="$kbuild_root/Makefile.modinc" cross_tools.comp
    test -f cross_tools.ko
)
