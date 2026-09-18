#!/bin/bash
# 为 ESP32-P4 v3.x 芯片编译所有应用（新工具链 20251107 + REV_MIN=301/REV_MAX=399，刷到 v1.0 板会被拒绝，防误刷）
cd "$(dirname "$0")" || exit 1
APPS="launcher retro-core prboom-go gwenesis neogeo fmsx gbsp"
for app in $APPS; do
    [ -f "$app/sdkconfig" ] || continue
    sed -i 's/CONFIG_ESP32P4_REV_MAX_FULL=199/CONFIG_ESP32P4_REV_MAX_FULL=302/; s/CONFIG_ESP_REV_MAX_FULL=199/CONFIG_ESP_REV_MAX_FULL=302/' $app/sdkconfig
done
export IDF_PATH=/home/lx/esp/esp-idf-553
export IDF_TOOLS_PATH=/home/lx/esp/idf-tools-55
source /home/lx/esp/export-553.sh > /dev/null 2>&1
python3 rg_tool.py --target p4-game build $APPS --no-networking
python3 rg_tool.py --target p4-game build-img $APPS --no-networking
