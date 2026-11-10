#!/usr/bin/env bash
# Black Ops III (Xbox 360) -> Linux static recompilation: full preparation pipeline.
#
#   tools/prepare.sh            # run every step (idempotent; skips extraction if already done)
#
# Steps:
#   1. extract the ZArchive disc image   -> extracted/game/
#   2. fetch + patch + build XenonRecomp -> tools/XenonRecomp/build/
#   3. build helper tools                -> tools/bo3prep/{bo3prep,bo3switch,ppcdis}
#   4. per executable (mp = default.xex, zm = default_zm.xex):
#        apply TU1 (.xexp), dump image, find save/restore helpers   (bo3prep)
#        detect jump tables + leaf function bounds                  (bo3switch)
#        generate config/BO3_<M>.toml                               (gen_config.py)
#        recompile PPC -> C++                                       (XenonRecomp) -> ppc/<m>/
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

ZAR="BKU-Call of Duty - Black Ops III.zar"
TU="content/0000000000000000/4156091D/000B0000/tu00000001_00000000"
XR=tools/XenonRecomp
XR_COMMIT=ddd128bcca99fe8bfbb99bea583c972351fa6ace

# setjmp / longjmp are located by hand (see README.md, "How the addresses were found").
declare -A SETJMP=( [mp]=0x82A60780 [zm]=0x82A52740 )
declare -A LONGJMP=( [mp]=0x82A11500 [zm]=0x82A03720 )

step() { printf '\n==> %s\n' "$*"; }

# 1 ---------------------------------------------------------------------------------------
if [ ! -f extracted/game/default.xex ]; then
    step "Extracting $ZAR"
    python3 -I tools/zar_extract.py "$ZAR" --verify
    python3 -I tools/zar_extract.py "$ZAR" extracted/game > /dev/null
fi

# 2 ---------------------------------------------------------------------------------------
if [ ! -x $XR/build/XenonRecomp/XenonRecomp ]; then
    step "Building XenonRecomp"
    if [ ! -d $XR/.git ]; then
        git clone --recursive https://github.com/hedge-dev/XenonRecomp.git $XR
    fi
    git -C $XR checkout -q $XR_COMMIT
    git -C $XR apply --check ../patches/xenonrecomp-bo3.patch 2>/dev/null && git -C $XR apply ../patches/xenonrecomp-bo3.patch
    cmake -S $XR -B $XR/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    cmake --build $XR/build -j"$(nproc)"
fi

# 3 ---------------------------------------------------------------------------------------
step "Building helper tools"
INC="-I$XR/XenonUtils -I$XR/thirdparty/simde -I$XR/thirdparty/disasm -I$XR/thirdparty/fmt/include"
LIBS="$XR/build/XenonUtils/libXenonUtils.a $XR/build/thirdparty/disasm/libdisasm.a $XR/build/thirdparty/fmt/libfmt.a"
for t in bo3prep bo3switch ppcdis; do
    clang++ -std=c++20 -O2 -o tools/bo3prep/$t tools/bo3prep/$t.cpp $INC $LIBS
done

# 4 ---------------------------------------------------------------------------------------
declare -A XEX=( [mp]=default.xex [zm]=default_zm.xex )
declare -A XEXP=( [mp]=default.xexp [zm]=default_zm.xexp )
for m in mp zm; do
    M=${m^^}
    step "[$M] preparing ${XEX[$m]}"
    mkdir -p private/$m config ppc/$m
    cp -n extracted/game/${XEX[$m]} private/$m/default.xex
    cp -n "$TU/${XEXP[$m]}" private/$m/default.xexp
    tools/bo3prep/bo3prep private/$m/default.xex private/$m/default.xexp private/$m
    tools/bo3prep/bo3switch private/$m/default_patched.xex config/BO3_${M}_switch_tables.toml config/BO3_${M}_functions.toml
    python3 -I tools/gen_config.py $m ${SETJMP[$m]} ${LONGJMP[$m]}

    step "[$M] recompiling"
    rm -f ppc/$m/*
    $XR/build/XenonRecomp/XenonRecomp config/BO3_${M}.toml $XR/XenonUtils/ppc_context.h \
        | grep -v '^Recompiling functions' | tee private/$m/recomp.log || true
    echo "[$M] $(ls ppc/$m/ppc_recomp.*.cpp | wc -l) source files, $(grep -c . private/$m/recomp.log || true) recompiler messages"
done

step "Done. Build the recompiled code with:"
echo "  cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build build"
