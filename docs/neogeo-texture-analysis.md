# NeoGeo 贴图渲染原理与异常分析

> **背景**：KOF97 显示正常，但合金弹头1（Metal Slug 1）贴图显示异常。
> **目标**：分析 gngeo 模拟器的贴图处理原理，定位可能导致游戏间显示差异的模拟器层面问题。

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
    int sectors_per_bank;     // 每 bank 的 512B 扇区数（通常 8）
    BYTE pdrv;                // FATFS 物理驱动器号
} GFX_CACHE;
```

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

**计算示例**（假设 `slot_size = 0x4000` = 16KB）：
```
slot_size >> 7 = 128        // 每个 bank 包含 128 个 tile
tile_sh = ~0x7F = 0xFFFFFF80

tileno = 0x12345
tileno & tile_sh = 0x12300
bank = 0x12300 / 128 = 367
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

### 问题 1：Tile 扩展后透明度检测不匹配 ⚠️⚠️⚠️

**严重程度**：高

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

### 问题 2：Tile 扩展与模运算冲突 ⚠️⚠️

**严重程度**：高

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

### 问题 3：缓存 Bank 越界 ⚠️⚠️

**严重程度**：高

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

### 问题 4：自动动画 Tile 号计算错误 ⚠️

**严重程度**：中

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

### 问题 5：spr_usage 表尺寸不足 ⚠️

**严重程度**：中

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

### 问题 6：预取与 VRAM 不同步 ⚠️

**严重程度**：低（设计权衡）

**位置**：`video.c:469-623`

**描述**：
预取在帧开始时读取 VRAM，但游戏代码可能在预取后、渲染前修改 VRAM，导致缓存内容与实际需求不匹配。

**影响**：偶发性贴图错误，下一帧自动纠正。

---

## 8. 游戏对比分析

| 特性 | KOF97 | 合金弹头1 |
|------|-------|-----------|
| **ROM 大小** | ~50MB | ~50MB+ |
| **Tile 数量** | < 0x10000 (65536) | > 0x10000 (可能使用扩展) |
| **Tile 扩展使用** | 较少/不使用 | 频繁 |
| **动态 sprite 密度** | 中等（角色为主） | 高（爆炸/子弹/敌人） |
| **VRAM 更新频率** | 中等 | 高 |
| **缓存未命中率** | 低 | 高 |
| **问题敏感度** | **低** | **高** |
| **表现** | 正常 | 贴图异常 |

### 为什么 KOF97 正常？

1. **Tile 数量可能 < 65536**：不触发扩展逻辑，所有 tile 号都在 `spr_usage` 有效范围内。
2. **动画使用较少**：即使有扩展，动画计算的副作用不明显。
3. **sprite 变化较缓**：缓存预取能跟上 VRAM 变化。

### 为什么合金弹头1异常？

1. **Tile 数量大**：频繁使用 tile 扩展，触发透明度检测越界。
2. **大量动态 sprite**：爆炸、子弹等高频更新，缓存压力大。
3. **动画 tile 密集**：动画计算与扩展逻辑冲突更明显。

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

#### 修复 1：扩展 Tile 透明度安全访问

```c
// 新增函数：安全获取透明度
static inline Uint8 get_pen_usage_safe(Uint32 tileno) {
    Uint32 max_index = memory.rom.spr_usage.size / sizeof(Uint32);
    Uint32 word_index = tileno >> 4;

    if (word_index >= max_index) {
        // 扩展 tile：使用基础 tile 的透明度或默认 NORMAL
        return TILE_NORMAL;
    }
    return ((((Uint32*)memory.rom.spr_usage.p)[word_index])
            >> ((tileno & 0xF) * 2)) & 0x3;
}

// 替换所有 PEN_USAGE(tileno) 调用为 get_pen_usage_safe(tileno)
```

#### 修复 2：缓存 Bank 越界保护

```c
// video.c - get_cached_sprite_ptr()
Uint8 *get_cached_sprite_ptr(Uint32 tileno) {
    GFX_CACHE *gcache = &memory.vid.spr_cache;
    int tile_sh = ~((gcache->slot_size >> 7) - 1);
    int bank = ((tileno & tile_sh) / (gcache->slot_size >> 7));

    // ✅ 新增：越界保护
    if (bank >= gcache->total_bank) {
        static int warn_cnt = 0;
        if (warn_cnt < 10) {
            printf("WARN: bank %d >= total %d (tileno=%u)\n",
                   bank, gcache->total_bank, tileno);
            warn_cnt++;
        }
        bank %= gcache->total_bank;  // 回退到有效范围
    }

    if (gcache->ptr[bank]) {
        return gcache->ptr[bank];
    }
    // ... 原有加载逻辑 ...
}
```

#### 修复 3：修正 Tile 扩展后的模运算

```c
// video_template.h - draw() 函数开头
// 移除或修正模运算，确保扩展 tile 不被错误映射

// 方案 A：移除模运算（如果上游已保证 tileno 有效）
// tileno = tileno % memory.nb_of_tiles;  // ← 删除此行

// 方案 B：使用安全的模运算
if (tileno >= memory.nb_of_tiles) {
    // 扩展 tile：不取模，直接使用（缓存系统会处理）
    // 或者标记为不可见
}
```

#### 修复 4：调整动画计算顺序

```c
// video.c - draw_screen()
// 先计算动画，再扩展

// 1. 先处理自动动画（使用基础 tile 号）
if (tileatr & 0x8) {
    tileno = (tileno & ~7) + ((tileno + neogeo_frame_counter) & 7);
} else if (tileatr & 0x4) {
    tileno = (tileno & ~3) + ((tileno + neogeo_frame_counter) & 3);
}

// 2. 然后处理 tile 扩展
if (memory.nb_of_tiles > 0x10000 && (tileatr & 0x10)) tileno += 0x10000;
if (memory.nb_of_tiles > 0x20000 && (tileatr & 0x20)) tileno += 0x20000;
if (memory.nb_of_tiles > 0x40000 && (tileatr & 0x40)) tileno += 0x40000;
```

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
