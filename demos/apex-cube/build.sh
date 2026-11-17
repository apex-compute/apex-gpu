#!/usr/bin/env bash
# Build vkcube with the Apex Compute logo as its texture.
# Fetches Khronos Vulkan-Tools, replaces vkcube's built-in LunarG texture with
# apex_compute_512.ppm and builds it into build/apex-cube/vkcube (relative to
# the apex-gpu folder). Needs git, cmake, ninja, python3 and a C compiler.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$ROOT/build/apex-cube
TOOLS=$OUT/Vulkan-Tools
TAG=${VULKAN_TOOLS_TAG:-v1.4.368}

mkdir -p "$OUT"
if [ ! -d "$TOOLS" ]; then
	git clone --depth 1 --branch "$TAG" https://github.com/KhronosGroup/Vulkan-Tools.git "$TOOLS"
fi

# vkcube embeds its texture as a P6 PPM in cube/lunarg.ppm.h (lunarg_ppm).
python3 - "$HERE/apex_compute_512.ppm" "$TOOLS/cube/lunarg.ppm.h" <<'EOF'
import sys
data = open(sys.argv[1], "rb").read()
if not data.startswith(b"P6\n"):
    sys.exit(f"{sys.argv[1]} is not a binary PPM (P6)")
lines = ["static unsigned char lunarg_ppm[] = {"]
for i in range(0, len(data), 21):
    lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 21]) + ",")
lines += ["};", f"static unsigned int lunarg_ppm_len = {len(data)};", ""]
open(sys.argv[2], "w").write("\n".join(lines))
EOF

cmake -S "$TOOLS" -B "$TOOLS/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DUPDATE_DEPS=ON -DBUILD_CUBE=ON -DBUILD_VULKANINFO=OFF -DBUILD_ICD=OFF \
	-DBUILD_WSI_WAYLAND_SUPPORT=OFF -DBUILD_WSI_DIRECTFB_SUPPORT=OFF
ninja -C "$TOOLS/build" vkcube
cp "$TOOLS/build/cube/vkcube" "$OUT/vkcube"
echo "built $OUT/vkcube"
