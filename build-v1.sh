#!/bin/bash
# 为 ESP32-P4 v1.0 芯片编译（旧工具链 + REV_MAX=199，刷到 v3.2 板会被 bootloader 拒绝，防误刷）
APP="${1:-neogeo}"
cd "$(dirname "$0")" || exit 1
sed -i 's/CONFIG_ESP32P4_REV_MAX_FULL=302/CONFIG_ESP32P4_REV_MAX_FULL=199/; s/CONFIG_ESP_REV_MAX_FULL=302/CONFIG_ESP_REV_MAX_FULL=199/' $APP/sdkconfig
export IDF_PATH=/home/lx/esp/esp-idf
export IDF_TOOLS_PATH=/home/lx/esp/idf-tools-55
source $IDF_PATH/export.sh > /dev/null 2>&1
python3 rg_tool.py --target p4-game build $APP --no-networking
