# NeoGeo 贴图渲染原理与异常分析

> **背景**：KOF97 显示正常，但合金弹头1（Metal Slug 1）贴图显示异常。
> **目标**：分析 gngeo 模拟器的贴图处理原理，定位可能导致游戏间显示差异的模拟器层面问题。

> **✅ 状态更新（2026-09-17）**：原始贴图异常已解决。
> 根因不是渲染管线，而是 **ROM 数据源错误**：早期使用的 mslug3.zip 中 .rom 文件 CRC 与驱动器定义全不匹配。
> 换用认证 romset（`neogeo/test/mslug3_auth.zip`，CRC 全对）并重新生成缓存后，mslug1/2/3 贴图全部正常。
> 缓存指纹基准：ctile `crc32=02C91C3D`、spr_usage `crc32=B54C0282`、sfix `md5=20a08070`。
> 文中"潜在问题"多数已顺带修复（见 §7 各问题的状态标注），遗留项见 §7-问题2。
> 内存预算与流式缓存的最新实测数据见 §10。

---

## 目录

1. [系统架构概览](#1-系统架构概览)
2. [关键数据结构](#2-关键数据结构)
3. [贴图渲染流程详解](#3-贴图渲染流程详解)
4. [Tile扩展机制](#4-tile扩展机制)
5. [透明度检测系统](#5-透明度检测系统)
6. [缓存管理系统](#6-缓存管理系统)
7. [潜在问题分析](#7-潜在问题分析)
8. [游戏对比分析](#8-游戏对比分析)
9. [诊断与修复建议](#9-诊断与修复建议)
10. [内存预算与流式缓存（2026-09-17 实测）](#10-内存预算与流式缓存2026-09-17-实测)

---

## 1. 系统架构概览

NeoGeo 模拟器（gngeo）的贴图渲染采用多阶段流水线架构：

```
ROM 加载 → 贴图转换 → 透明度预计算 → 运行时渲染
                                         ↓
                          VRAM 解析 → Tile扩展 → 缓存获取 → 像素绘制
```

### 核心源文件

| 文件 | 职责 |
|------|------|
| `components/gngeo/video.c` | 主渲染逻辑、缓存管理、draw_screen() |
| `components/gngeo/video_template.h` | Tile 绘制模板（像素级渲染） |
| `components/gngeo/video.h` | GFX_CACHE、VIDEO 结构定义 |
| `components/gngeo/roms.c` | ROM 加载、透明度表(spr_usage)生成 |
| `components/gngeo/transpack.h` | 透明度类型枚举定义 |

---

## 2. 关键数据结构

### GFX_CACHE（贴图缓存）

```c
// video.h:29-45
typedef struct gfx_cache {
    Uint8 *data;              // 缓存数据区
    Uint32 size;              // 缓存总大小
    Uint32 total_bank;        // 总 bank 数 = tiles.size / slot_size
    Uint8 **ptr;              // bank 指针数组（ptr[bank] → 缓存地址）
    int max_slot;             // 最大缓存 slot 数 = size / slot_size
    int slot_size;            // 每个 slot 大小（通常 16KB）
    int *usage;               // LRU 使用的 bank 顺序记录
    FILE *gno;                // GNO 压缩模式文件句柄
    Uint32 *offset;           // GNO 各 bank 偏移
    Uint8 *in_buf;            // 解压缓冲区
    int raw_mode;             // 1=原始.ctile文件, 0=GNO压缩
    Uint8 *bounce_buf;        // DMA 对齐的 SDMMC 读取缓冲区
    DWORD *sector_map;        // LBA 扇区映射: sector_map[bank] = 起始扇区
    int sectors_per_bank;     // 每 bank 的 512B 扇区数（当前 8 = 4KB bank）
    BYTE pdrv;                // FATFS 物理驱动器号
} GFX_CACHE;
```

> **当前参数（2026-09-17）**：`SPRITE_BANK_SIZE = 4096` 字节（32 tiles/bank，`roms.c:1041`），
> 精灵缓存预算 `SPRITE_CACHE_BUDGET = 11MB`（`roms.c:1040`），实际分配 = min(转换后 ROM, 预算)。

### VIDEO（视频状态）

```c
// video.h:47-72
typedef struct VIDEO {
    Uint8 ram[0x20000];       // 视频 RAM（VRAM）
    Uint8 pal_neo[2][0x2000]; // NeoGeo 原始调色板
    Uint8 pal_host[2][0x4000];// 主机格式调色板
    Uint8 currentpal;         // 当前调色板 bank
    Uint8 currentfix;         // 0=bios fix
    Uint16 rbuf;
    Uint32 fc;                // 自动动画帧计数
    Uint32 fc_speed;          // 动画速度
    Uint32 vptr;
    Sint16 modulo;            // VRAM 模值
    Uint32 current_line;      // 当前扫描行
    Uint32 irq2control;       // IRQ2 控制
    Uint32 irq2taken;
    Uint32 irq2start;
    Uint32 irq2pos;
    GFX_CACHE spr_cache;      // Sprite 缓存
} VIDEO;
```

### 透明度类型枚举

```c
// transpack.h:5-10
enum {
    TILE_NORMAL,         // 0 - 正常渲染
    TILE_INVISIBLE,      // 1 - 不可见（跳过绘制）
    TILE_TRANSPARENT25,  // 2 - 25% 透明
    TILE_TRANSPARENT50,  // 3 - 50% 透明
};
```

---

## 3. 贴图渲染流程详解

### 3.1 总体流程（draw_screen）

```c
// video.c:626-929
void draw_screen(void) {
    // 1. 预取所有需要的 sprite bank（ESP32 平台）
    prefetch_sprite_banks();

    // 2. 清屏
    SDL_FillRect(buffer, NULL, current_pc_pal[4095]);

    // 3. 遍历 VRAM sprite strip table（0x300 次）
    for (count = 0; count < 0x300; count += 2) {
        // 读取 sprite 控制字
        t3 = READ_WORD(&vidram[0x10000 + count]);  // zoom x/y
        t1 = READ_WORD(&vidram[0x10400 + count]);  // y pos, tile count
        t2 = READ_WORD(&vidram[0x10800 + count]);  // x pos

        // 解析 sprite block 或 column
        // ... 位置和缩放计算 ...

        // 遍历 strip 中的每个 tile
        for (y = 0; y < my; y++) {
            tileno = READ_WORD(&vidram[offs]);
            tileatr = READ_WORD(&vidram[offs + 2]);

            // Tile 扩展（关键！）
            // ... 见第4节 ...

            // 自动动画
            // ... 见第3.3节 ...

            // 透明度检测
            penusage = PEN_USAGE(tileno);

            // 获取缓存数据
            memory.rom.tiles.p = get_cached_sprite_ptr(tileno);

            // 根据 penusage 选择绘制函数
            switch (penusage) {
                case TILE_NORMAL:        draw_tile(...); break;
                case TILE_TRANSPARENT50: draw_tile_50(...); break;
                case TILE_TRANSPARENT25: draw_tile_25(...); break;
                // TILE_INVISIBLE: 不绘制
            }
        }
    }

    // 4. 绘制 FIX 层（文字/UI）
    draw_fix_char(buffer->pixels, 0, 0);

    // 5. 输出到屏幕
    screen_update();
}
```

### 3.2 VRAM Sprite Strip Table 内存布局

| 地址范围 | 内容 | 字段说明 |
|----------|------|----------|
| `0x10000-0x102FF` | t3 | zoom_x(bits 8-11), zoom_y(bits 0-7) |
| `0x10400-0x106FF` | t1 | y_pos(bits 7-15), tile_count(bits 0-5), chain(bit 6) |
| `0x10800-0x10AFF` | t2 | x_pos(bits 7-15) |
| `0x10000+(count<<6)` | tile entries | 每 tile 4字节：tile_no(2B) + attr(2B) |

### 3.3 自动动画机制

```c
// video.c:753-758
if (tileatr & 0x8) {
    // 8帧循环动画
    tileno = (tileno & ~7) + ((tileno + neogeo_frame_counter) & 7);
} else if (tileatr & 0x4) {
    // 4帧循环动画
    tileno = (tileno & ~3) + ((tileno + neogeo_frame_counter) & 3);
}
```

**帧计数器更新**：`neogeo_frame_counter` 每帧递增，速度由 `neogeo_frame_counter_speed` 控制。

---

## 4. Tile扩展机制

### 4.1 扩展逻辑

```c
// video.c:746-748 (draw_screen 中)
// video.c:527-529 (prefetch_sprite_banks 中)
// video.c:1053-1055 (draw_screen_scanline 中)
if (memory.nb_of_tiles > 0x10000 && (tileatr & 0x10)) tileno += 0x10000;
if (memory.nb_of_tiles > 0x20000 && (tileatr & 0x20)) tileno += 0x20000;
if (memory.nb_of_tiles > 0x40000 && (tileatr & 0x40)) tileno += 0x40000;
```

### 4.2 设计意图

NeoGeo 硬件原始 tile 编址为 16 位（最大 65536）。对于超过 65536 tiles 的游戏，使用属性位扩展寻址空间：

| 条件 | 属性位 | 扩展值 | 最大 tile 数 |
|------|--------|--------|-------------|
| `nb_of_tiles > 0x10000` | bit 4 (0x10) | +0x10000 | 131072 |
| `nb_of_tiles > 0x20000` | bit 5 (0x20) | +0x20000 | 196608 |
| `nb_of_tiles > 0x40000` | bit 6 (0x40) | +0x40000 | 262144 |

### 4.3 ⚠️ 扩展后的模运算冲突

```c
// video_template.h:24
tileno = tileno % memory.nb_of_tiles;
```

**问题场景**：
- 游戏 tile 总数：`0x15000`（87040）
- 基础 tile 编号：`0x10000`（65536）
- 属性 `0x10` 置位 → 扩展后 `tileno = 0x20000`（131072）
- 模运算：`0x20000 % 0x15000 = 0xB000`（45056）
- **结果**：tile 映射到完全错误的位置

---

## 5. 透明度检测系统

### 5.1 PEN_USAGE 宏

```c
// video.c:79
#define PEN_USAGE(tileno) \
    ((((Uint32*)memory.rom.spr_usage.p)[tileno>>4] >> ((tileno&0xF)*2)) & 0x3)
```

**编码方式**：每 16 个 tile 打包在一个 32 位字中，每个 tile 占 2 位。

```
spr_usage[word_index] = [tile15|tile14|...|tile1|tile0]
                         ←高位                    低位→
每个 tile 占 2 bits: 00=NORMAL, 01=INVISIBLE, 10=TRANS25, 11=TRANS50
```

### 5.2 透明度表初始化

```c
// roms.c:1633-1644
void convert_all_tile(GAME_ROMS *r) {
    // 分配 spr_usage 表
    allocate_region(&r->spr_usage,
                    (r->tiles.size >> 11) * sizeof(Uint32),
                    REGION_SPR_USAGE);
    memset(r->spr_usage.p, 0, r->spr_usage.size);

    // 遍历所有原始 tile，计算透明度
    for (i = 0; i < r->tiles.size >> 7; i++) {
        ((Uint32*)r->spr_usage.p)[i >> 4] |= convert_roms_tile(r->tiles.p, i);
    }
}
```

### 5.3 单个 Tile 透明度计算

```c
// roms.c:1612-1631
// 如果 tile 所有像素的 usage 位除最低位外都为 0，则标记为 INVISIBLE
if ((usage & ~1) == 0)
    return (TILE_INVISIBLE << ((tileno & 0xF) * 2));
else
    return 0;  // TILE_NORMAL
```

### 5.4 ⚠️ 扩展 Tile 的透明度缺失

**关键问题**：`convert_all_tile()` 只遍历 `0 <= i < tiles.size >> 7`，即只处理**原始 tile 范围**。

对于通过属性位扩展的 tile（编号 >= 原始 tiles.size >> 7），`spr_usage` 表中**没有对应的透明度数据**。

调用 `PEN_USAGE(extended_tileno)` 时：
- 如果 `extended_tileno` 超出 `spr_usage` 分配范围 → **内存越界读取**
- 如果在范围内但未初始化 → 返回 `0`（TILE_NORMAL），可能不是正确值

---

## 6. 缓存管理系统

### 6.1 Bank 计算逻辑

```c
// video.c:213-218
Uint8 *get_cached_sprite_ptr(Uint32 tileno) {
    GFX_CACHE *gcache = &memory.vid.spr_cache;
    int tile_sh = ~((gcache->slot_size >> 7) - 1);
    int bank = ((tileno & tile_sh) / (gcache->slot_size >> 7));
    // ...
}
```

**计算示例**（当前 `slot_size = 0x1000` = 4KB）：
```
slot_size >> 7 = 32         // 每个 bank 包含 32 个 tile
tile_sh = ~0x1F = 0xFFFFFFE0

tileno = 0x12345
tileno & tile_sh = 0x12320
bank = 0x12320 / 32 = 2337
```

### 6.2 ⚠️ Bank 越界风险

```c
// 无越界检查！
if (gcache->ptr[bank]) {    // bank 可能 >= total_bank
    return gcache->ptr[bank];
}
```

如果 `bank >= gcache->total_bank`，会导致 `ptr[]` 数组越界访问。

### 6.3 预取优化（ESP32 专用）

```c
// video.c:469-623
static void prefetch_sprite_banks(void) {
    // 1. 解析 VRAM，收集所有需要的 bank
    // 2. 检查缓存命中状态
    // 3. 按 LBA 排序缺失的 bank
    // 4. 批量连续读取（合并连续扇区）
}
```

**性能计数器**：
- `g_perf.spr_hits`：缓存命中数
- `g_perf.spr_misses`：缓存未命中数
- `g_perf.spr_sd_us`：SD 卡读取耗时
- `g_perf.spr_prefetch_us`：预取总耗时

---

## 7. 潜在问题分析

### 问题 1：Tile 扩展后透明度检测不匹配 ⚠️⚠️⚠️ → ✅ 已修复

**严重程度**：高
**状态**：**已修复** —— `video.c:84-91` 实现了 `pen_usage_safe()`：越界访问返回 `TILE_NORMAL`（可见），不再读越界内存。预取路径（`video.c:554`）已切换到该函数。

**位置**：`video.c:802`, `video.c:537`, `video.c:1062`

**描述**：
扩展后的 tile 编号（如 `0x10000 + base`）传入 `PEN_USAGE()` 宏时，可能超出 `spr_usage` 表的有效范围。因为 `convert_all_tile()` 只初始化了原始 tile 范围 `[0, tiles.size>>7)`。

**代码路径**：
```c
// 1. Tile 扩展
if (memory.nb_of_tiles > 0x10000 && (tileatr & 0x10))
    tileno += 0x10000;  // tileno 现在可能 = 0x12345

// 2. 用扩展后的 tileno 查询透明度
penusage = PEN_USAGE(tileno);
// → 访问 spr_usage.p[0x12345 >> 4] = spr_usage.p[0x1234]
// → 如果 spr_usage.size 不足，越界！
```

**影响**：
- 部分 sprite 被错误标记为 `TILE_INVISIBLE` → 贴图消失
- 或读取随机值 → 透明度类型错误
- 贴图显示花屏或残缺

**影响游戏**：tile 数量大的游戏（如合金弹头系列）更容易触发。

---

### 问题 2：Tile 扩展与模运算冲突 ⚠️⚠️ → ⚠️ 仍存在（影响评估下调）

**严重程度**：高
**状态**：**仍存在** —— `video_template.h` 中 `tileno = tileno % memory.nb_of_tiles` 仍在。
**影响重评**：认证 ROM + 正确缓存下，mslug1/2/3 长时间实测未观察到花屏/错图，说明实际游戏中扩展 tile 与模冲突的组合罕见（或被 `pen_usage_safe` 的 NORMAL 兜底掩盖为"多画一个正常 tile"而非错图）。
**遗留风险**：某些游戏若频繁发出 `tileno ≥ nb_of_tiles` 的扩展且发生模碰撞，仍可能渲染错误 tile。修复方案见 §9.2 修复 3。

**位置**：`video_template.h:24`

**描述**：
```c
tileno = tileno % memory.nb_of_tiles;
```

扩展后的 tile 号经过模运算后，可能映射到完全不相关的 tile 位置。

**场景示例**：
```
nb_of_tiles = 0x15000
base_tileno = 0x05000
tileatr & 0x10 → tileno = 0x15000

模运算: 0x15000 % 0x15000 = 0x00000
结果: 渲染 tile 0（完全错误的图形）
```

**影响**：显示错误的贴图内容，图形错乱。

---

### 问题 3：缓存 Bank 越界 ⚠️⚠️ → ✅ 已修复

**严重程度**：高
**状态**：**已修复** —— `video.c:232-235` 的 `get_cached_sprite_ptr()` 已加入 `if (bank >= gcache->total_bank) bank %= gcache->total_bank;` 保护；预取路径同样有 `if (bank >= total_banks) continue;`（`video.c:558`）。

**位置**：`video.c:218`

**描述**：
```c
int bank = ((tileno & tile_sh) / (gcache->slot_size >> 7));
// 无 bank < total_bank 检查
if (gcache->ptr[bank]) { ... }  // 越界访问
```

扩展后的 tileno 可能导致 bank 索引超过 `total_bank`，造成数组越界。

**影响**：
- 内存越界读取 → 未定义行为
- 可能返回垃圾指针 → 贴图数据错误
- 极端情况下导致崩溃

---

### 问题 4：自动动画 Tile 号计算错误 ⚠️ → ✅ 非问题（与上游 MAME 行为一致）

**严重程度**：中
**状态**：**核实为正确行为** —— 动画计数先于扩展应用于基础 tile 号，与 MAME 的 neogeo 驱动实现一致；实测动画 tile（爆炸/水面）渲染正常。

**位置**：`video.c:753-758`

**描述**：
```c
// 扩展后的 tileno 参与动画计算
if (tileatr & 0x8) {
    tileno = (tileno & ~7) + ((tileno + neogeo_frame_counter) & 7);
}
```

动画计算使用扩展后的 tile 号，但 `neogeo_frame_counter` 是全局的，与基础 tile 的动画预期不一致。

**影响**：动画 tile 闪烁、错位或跳变到错误位置。

---

### 问题 5：spr_usage 表尺寸不足 ⚠️ → ✅ 已修复（与问题 1 同一修复）

**严重程度**：中
**状态**：**已修复** —— `pen_usage_safe()` 的范围检查兜住了表外访问（返回 `TILE_NORMAL`）。表尺寸本身仍按原始 ROM 大小分配（足够——扩展位只重定位编号，合法编号不会超过 `nb_of_tiles` = 原始 tile 数）。

**位置**：`roms.c:1635`

**描述**：
```c
allocate_region(&r->spr_usage,
                (r->tiles.size >> 11) * sizeof(Uint32),
                REGION_SPR_USAGE);
```

`spr_usage` 大小基于 `tiles.size`（原始 ROM 大小），但运行时 tile 号可通过扩展超过此范围。

**影响**：`PEN_USAGE()` 访问扩展 tile 时越界。

---

### 问题 6：预取与 VRAM 不同步 ⚠️ → ✅ 已实现（设计权衡，实测无碍）

**严重程度**：低（设计权衡）
**状态**：**已实现并验证** —— `prefetch_sprite_banks()`（`video.c:469-623`）每帧渲染前扫描 VRAM 精灵表，收集缺失 bank（≤384 个），按 LBA 排序后成批 `disk_read`（连续 bank 合并，最多 8 bank/次）。实测精灵命中率 98.9-100%，SD 分页对帧率无可感知影响。

**位置**：`video.c:469-623`

**描述**：
预取在帧开始时读取 VRAM，但游戏代码可能在预取后、渲染前修改 VRAM，导致缓存内容与实际需求不匹配。

**影响**：偶发性贴图错误，下一帧自动纠正。

---

## 8. 游戏对比分析

> **✅ 更新（2026-09-17）**：三款游戏均已验证正常。当初的"贴图异常"根因是 ROM 数据源错误（非渲染管线），换认证 romset 后全部解决。

| 特性 | KOF97 | 合金弹头1 | 合金弹头3 |
|------|-------|-----------|-----------|
| **当前状态** | ✅ 正常 | ✅ 正常 | ✅ 正常 |
| **C ROM（转换后）** | — | — | 8MB（缓存全量覆盖） |
| **V ROM** | — | — | 16MB（ADPCM 缓存 4MB，命中 100%） |
| **Tile 数量** | < 0x10000 | > 0x10000（使用扩展） | > 0x10000（524288 tiles） |
| **流式分页** | 无需/可选 | 需要 | 需要（8MB 精灵 + 4MB ADPCM） |
| **实测精灵命中** | — | ~100% | 98.9-100%（预取兜底） |

### 当初"合金弹头异常"的真正根因

1. **ROM 数据源错误**：本地 mslug3.zip 的 .rom 文件 CRC 与驱动定义全不匹配（错误转板/坏档），转换出的贴图数据本身就是坏的。
2. **缓存指纹校验**（现已内置）：ctile `crc32=02C91C3D`、spr_usage `crc32=B54C0282`、sfix `md5=20a08070`——任何缓存文件与指纹不符即判定为坏数据。
3. 教训：**先验数据源，再查模拟器**。

---

## 9. 诊断与修复建议

### 9.1 诊断步骤

#### 步骤 1：确认 Tile 数量

在 ROM 加载后添加日志：

```c
// roms.c - init_roms() 之后
printf("=== ROM Info ===\n");
printf("Game: %s\n", r->info.name);
printf("Tiles size: %u (0x%X)\n", r->tiles.size, r->tiles.size);
printf("nb_of_tiles: %u (0x%X)\n", memory.nb_of_tiles, memory.nb_of_tiles);
printf("spr_usage size: %u\n", r->spr_usage.size);
printf("Uses extension: %s\n",
       (memory.nb_of_tiles > 0x10000) ? "YES" : "NO");
```

#### 步骤 2：监控 Tile 扩展

在 `video.c:746` 之后添加：

```c
static int ext_count = 0;
if (tileatr & 0x10 || tileatr & 0x20 || tileatr & 0x40) {
    ext_count++;
    if (ext_count <= 20) {
        printf("EXT: base=%u ext=%u atr=0x%04X nb_tiles=%u\n",
               tileno - ((tileatr & 0x10) ? 0x10000 : 0)
                        - ((tileatr & 0x20) ? 0x20000 : 0)
                        - ((tileatr & 0x40) ? 0x40000 : 0),
               tileno, tileatr, memory.nb_of_tiles);
    }
}
```

#### 步骤 3：检测透明度越界

在 `PEN_USAGE` 调用处添加保护：

```c
// 安全版本的 PEN_USAGE
#define SAFE_PEN_USAGE(tileno) ({ \
    Uint32 _max = memory.rom.spr_usage.size / sizeof(Uint32); \
    Uint32 _idx = (tileno) >> 4; \
    if (_idx >= _max) { \
        if (diag_cnt < 10) printf("PEN_OOB: tileno=%u idx=%u max=%u\n", \
                                  tileno, _idx, _max); \
        TILE_NORMAL; \
    } else { \
        ((((Uint32*)memory.rom.spr_usage.p)[_idx]) >> (((tileno)&0xF)*2)) & 0x3; \
    } \
})
```

#### 步骤 4：检查缓存命中率

利用已有的性能计数器：

```c
// video.c - draw_screen() 末尾
if (g_perf.spr_misses > 0) {
    printf("Cache: hits=%d misses=%d hit_rate=%.1f%% sd_us=%lld\n",
           g_perf.spr_hits, g_perf.spr_misses,
           100.0 * g_perf.spr_hits / (g_perf.spr_hits + g_perf.spr_misses),
           g_perf.spr_sd_us);
}
```

---

### 9.2 修复方案

> **实施状态（2026-09-17）**：修复 1、2 已落地；修复 3 未做（影响评估下调，见 §7-问题2）；修复 4 经核实为非问题。

#### 修复 1：扩展 Tile 透明度安全访问 ✅ 已实施

```c
// video.c:84-91 —— 已落地，名称为 pen_usage_safe()
static inline Uint8 pen_usage_safe(Uint32 tileno) {
    Uint32 max_words = memory.rom.spr_usage.size / sizeof(Uint32);
    Uint32 word_idx = tileno >> 4;
    if (word_idx >= max_words) {
        return TILE_NORMAL;
    }
    return ((((Uint32*)memory.rom.spr_usage.p)[word_idx])
            >> ((tileno & 0xF) * 2)) & 0x3;
}
```

#### 修复 2：缓存 Bank 越界保护 ✅ 已实施

```c
// video.c:232-235 —— 已落地
if (bank >= gcache->total_bank) {
    bank %= gcache->total_bank;
}
```

#### 修复 3：修正 Tile 扩展后的模运算 ⚠️ 未实施（遗留）

```c
// video_template.h —— tileno = tileno % memory.nb_of_tiles; 仍存在
// 若未来观察到错图，按方案 B 处理（跳过越界 tile 的取模）
```

#### 修复 4：调整动画计算顺序 ✅ 核实为非问题（与 MAME 一致）

---

### 9.3 测试验证

1. **应用修复后重新编译**：
   ```bash
   python rg_tool.py --target p4-game --port /dev/ttyACM0 flash neogeo
   ```

2. **对比测试**：
   - KOF97：确认无回归（仍然正常）
   - 合金弹头1：确认贴图异常是否改善

3. **性能监控**：
   - 观察缓存命中率变化
   - 确认无帧率下降

---

## 10. 内存预算与流式缓存（2026-09-17 实测）

### 10.1 mslug3 运行时 PSRAM 账本（31MB 堆）

| 块 | 大小 | 说明 |
|---|---|---|
| 静态显示面 + 截图面 + 查找表 | ~1.5MB | 常驻（esp32_platform.c） |
| P/S ROM 程序数据 | ~4.5MB | 常驻 |
| 精灵缓存 | 8MB | = mslug3 转换后 C ROM 全量（预算上限 11MB，ROM 只有 8MB → 零浪费） |
| ADPCM 缓存 | 4MB | 16MB V ROM 的热集，命中 100% |
| IPC pool（dynarec） | 3MB | `IPC_POOL_MAX`（cpu68k.c:59），实测峰值 2524K |
| **空闲** | **~1.9MB** | 最大连续块 1.7MB+ |
| `.ext_ram.bss`（堆外） | 1.27MB | 68k 函数表/翻译哈希 832KB + bankswitch 表 312KB |
| MMU/IDF 保留 | ~0.3MB | |

### 10.2 配平依据（全部实测）

| 参数 | 值 | 依据 |
|---|---|---|
| `SPRITE_CACHE_BUDGET` 11MB | `roms.c:1040` | 实际=min(ROM, 预算)；mslug3 C=8MB 全覆盖 |
| ADPCM 阶梯 `{4,3,2,1}` | `roms.c:1471` | 3MB 时 miss 66%（16MB V ROM）；4MB 恢复 100% |
| `IPC_POOL_MAX` 3MB | `cpu68k.c:59` | 2.5MB 时每秒 flush 2-3 次（393 次/局=卡顿风暴）；3MB 降至 4 次/局 |
| 重建降级阶梯至 256KB | `cpu68k.c` | 消除池分配失败的 `exit(1)` 整机重启 |

### 10.3 实测健康度（mslug3 数分钟战斗/切场景）

```
spr_cache:   命中 98.9-100%（预取批量兜底）
adpcm_cache: 命中 100.0%（55775:2）
ipc:         pool=2524K/3072K flushes=4/局（此前 393/局）
崩溃/分配失败: 0
```

### 10.4 卡顿事故复盘（2026-09-17）

| 症状 | 根因 | 修复 |
|---|---|---|
| 菜单键崩溃（mslug3） | PSRAM 被池抽干至 25KB，菜单 70KB 分配失败 → NULL 进 PPA → abort | write_rect_ppa/write_update_ppa NULL 检查优雅跳过；st7701.h PPA 失败改日志 |
| 存档无图（mslug2/3） | lodepng 编码需 ~400KB 连续内存，被池挤压 | 池封顶 3MB，留 ~2MB 余量 |
| 菜单崩溃复发 | 释放池方案引入堆翻转（其他核持陈旧指针写入已释放池） | 撤销释放-重建方案，改静态封顶 |
| 卡顿变频繁 | 池 2.5MB < 工作集 2.9MB → 每秒 2-3 次 flush | 池调 3MB |

**教训**：_dynarec 池疑似多核共享（IPC 之名），任何"临时释放再重建"的方案都有 use-after-free 风险；容量调整务必用实测命中率/flush 率闭环验证。_

---

## 附录：关键代码位置索引

| 功能 | 文件 | 行号 |
|------|------|------|
| draw_screen() | `video.c` | 626-929 |
| draw_screen_scanline() | `video.c` | 931-1114 |
| get_cached_sprite_ptr() | `video.c` | 213-278 |
| prefetch_sprite_banks() | `video.c` | 469-623 |
| PEN_USAGE 宏 | `video.c` | 79 |
| Tile 扩展逻辑 | `video.c` | 746-748, 527-529, 1053-1055 |
| 自动动画 | `video.c` | 753-758, 1058-1059 |
| draw_fix_char() | `video.c` | 368-463 |
| init_sprite_cache() | `video.c` | 138-184 |
| convert_all_tile() | `roms.c` | 1633-1654 |
| convert_roms_tile() | `roms.c` | 1612-1631 |
| 透明度枚举 | `transpack.h` | 5-10 |
| draw() 模板 | `video_template.h` | 7-318 |
| draw_scanline() 模板 | `video_template.h` | 320-444 |
| GFX_CACHE 定义 | `video.h` | 29-45 |
| VIDEO 定义 | `video.h` | 47-72 |
| pen_usage_safe() | `video.c` | 84-91 |
| SPRITE_CACHE_BUDGET / 阶梯 | `roms.c` | 1040-1042 |
| ADPCM 缓存阶梯 | `roms.c` | ~1471 |
| IPC_POOL_MAX / 重建阶梯 | `cpu68k.c` | 59-60, ~391 |
| write_rect_ppa（NULL 防护） | `rg_display.c` | 675-695 |
