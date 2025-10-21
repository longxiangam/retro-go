#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_st7701.h>
#include <esp_ldo_regulator.h>

// MIPI DSI 配置
#ifndef RG_MIPI_DSI_LANE_NUM
#define RG_MIPI_DSI_LANE_NUM          1     // 数据通道数
#endif

#ifndef RG_MIPI_DSI_LANE_BITRATE_MBPS
#define RG_MIPI_DSI_LANE_BITRATE_MBPS 1000  // 1Gbps
#endif

#ifndef RG_MIPI_DSI_DPI_CLK_MHZ
#define RG_MIPI_DSI_DPI_CLK_MHZ       32    // DPI 时钟频率
#endif

// 时序参数
#ifndef RG_MIPI_DSI_LCD_HSYNC
#define RG_MIPI_DSI_LCD_HSYNC         10
#endif
#ifndef RG_MIPI_DSI_LCD_HBP
#define RG_MIPI_DSI_LCD_HBP           10
#endif
#ifndef RG_MIPI_DSI_LCD_HFP
#define RG_MIPI_DSI_LCD_HFP           20
#endif
#ifndef RG_MIPI_DSI_LCD_VSYNC
#define RG_MIPI_DSI_LCD_VSYNC         10
#endif
#ifndef RG_MIPI_DSI_LCD_VBP
#define RG_MIPI_DSI_LCD_VBP           10
#endif
#ifndef RG_MIPI_DSI_LCD_VFP
#define RG_MIPI_DSI_LCD_VFP           10
#endif

// LDO 配置（用于 MIPI DSI PHY 供电）
#ifndef RG_MIPI_DSI_PHY_PWR_LDO_CHAN
#define RG_MIPI_DSI_PHY_PWR_LDO_CHAN       3     // LDO 通道
#endif
#ifndef RG_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV
#define RG_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500  // 2.5V
#endif

// ST7701 初始化命令序列
static const st7701_lcd_init_cmd_t st7701_init_cmds[] = {
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t []){0x08}, 1, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t []){0x4F, 0x00}, 2, 0},
    {0xC1, (uint8_t []){0x0C, 0x02}, 2, 0},
    {0xC2, (uint8_t []){0x37, 0x06}, 2, 0},
    {0xB0, (uint8_t []){0x40, 0xC3, 0x96, 0x0C, 0x12, 0x08, 0x07, 0x06, 0x06, 0x20, 0x03, 0x92, 0x10, 0x66, 0xAF, 0x9D}, 16, 0},
    {0xB1, (uint8_t []){0x40, 0xC3, 0x95, 0x0E, 0x11, 0x07, 0x0A, 0x09, 0x09, 0x22, 0x05, 0x11, 0x0F, 0x28, 0xEE, 0xD9}, 16, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t []){0x3D}, 1, 0},
    {0xB1, (uint8_t []){0x7F}, 1, 0},
    {0xB2, (uint8_t []){0x89}, 1, 0},
    {0xB3, (uint8_t []){0x80}, 1, 0},
    {0xB5, (uint8_t []){0x49}, 1, 0},
    {0xB7, (uint8_t []){0x85}, 1, 0},
    {0xB8, (uint8_t []){0x23}, 1, 0},
    {0xC1, (uint8_t []){0x78}, 1, 0},
    {0xC2, (uint8_t []){0x78}, 1, 0},
    {0xD0, (uint8_t []){0x88}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x00, 0x02}, 3, 0},
    {0xE1, (uint8_t []){0x07, 0xC0, 0x07, 0xC0, 0x06, 0xC0, 0x06, 0xC0, 0x00, 0x24, 0x24}, 11, 0},
    {0xE2, (uint8_t []){0x20, 0x20, 0x22, 0x22, 0x8D, 0xC0, 0x00, 0x00, 0x8C, 0xC0, 0x00, 0x00, 0x00}, 13, 0},
    {0xE3, (uint8_t []){0x00, 0x00, 0x22, 0x22}, 4, 0},
    {0xE4, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t []){0x09, 0x91, 0x10, 0xFA, 0x0B, 0x93, 0x10, 0xFA, 0x0D, 0x8D, 0x10, 0xFA, 0x0F, 0x8F, 0x10, 0xFA}, 16, 0},
    {0xE6, (uint8_t []){0x00, 0x00, 0x22, 0x22}, 4, 0},
    {0xE7, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t []){0x08, 0x90, 0x10, 0xFA, 0x0A, 0x92, 0x10, 0xFA, 0x0C, 0x8C, 0x10, 0xFA, 0x0E, 0x8E, 0x10, 0xFA}, 16, 0},
    {0xEB, (uint8_t []){0x02, 0x01, 0xE4, 0xE4, 0x88, 0x00, 0x00}, 7, 0},
    {0xED, (uint8_t []){0xFF, 0x04, 0x56, 0x7F, 0xBA, 0x2F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF2, 0xAB, 0xF7, 0x65, 0x40, 0xFF}, 16, 0},
    {0xEF, (uint8_t []){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
   // {0x21, (uint8_t []){0x00}, 0, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x29, (uint8_t []){0x00}, 0, 0},
};

// 全局变量
static esp_lcd_panel_handle_t mipi_dpi_panel = NULL;
static esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
static esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
static esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
static SemaphoreHandle_t transfer_done_sem = NULL;

// 缓冲区队列
static QueueHandle_t lcd_buffers;
#define LCD_BUFFER_COUNT 5
static uint16_t *lcd_buffer_pool[LCD_BUFFER_COUNT];

// 存储当前窗口状态
static struct {
    int left;
    int top;
    int width;
    int height;
    bool valid;
} current_window = {0};



// 传输完成回调
static bool lcd_color_trans_done_cb(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    BaseType_t high_task_awoken = pdFALSE;
    if (transfer_done_sem) {
        xSemaphoreGiveFromISR(transfer_done_sem, &high_task_awoken);
    }
    return high_task_awoken == pdTRUE;
}

static void lcd_init(void)
{
    esp_err_t ret;

    RG_LOGI("Initializing ST7701 MIPI DSI LCD driver\n");

    // 创建传输完成信号量
    transfer_done_sem = xSemaphoreCreateBinary();
    RG_ASSERT(transfer_done_sem, "Failed to create transfer semaphore");

    // 初始化缓冲区队列
    lcd_buffers = xQueueCreate(LCD_BUFFER_COUNT, sizeof(uint16_t *));
    RG_ASSERT(lcd_buffers, "Failed to create buffer queue");

    for (int i = 0; i < LCD_BUFFER_COUNT; i++) {
        lcd_buffer_pool[i] = rg_alloc(LCD_BUFFER_LENGTH * 2, MEM_DMA);
        RG_ASSERT(lcd_buffer_pool[i], "Failed to allocate LCD buffer");
        xQueueSend(lcd_buffers, &lcd_buffer_pool[i], portMAX_DELAY);
    }

    // 1. 启用 MIPI DSI PHY 电源
#ifdef RG_MIPI_DSI_PHY_PWR_LDO_CHAN
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = RG_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = RG_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ret = esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy);
    RG_ASSERT(ret == ESP_OK, "Failed to acquire LDO channel for MIPI DSI PHY");
    RG_LOGI("MIPI DSI PHY powered on\n");
#endif

    // 2. 创建 MIPI DSI 总线
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = RG_MIPI_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = RG_MIPI_DSI_LANE_BITRATE_MBPS,
    };
    ret = esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);
    RG_ASSERT(ret == ESP_OK, "Failed to create MIPI DSI bus");
    RG_LOGI("MIPI DSI bus created\n");

    // 3. 创建 MIPI DBI IO (用于发送命令)
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ret = esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io);
    RG_ASSERT(ret == ESP_OK, "Failed to create MIPI DBI IO");
    RG_LOGI("MIPI DBI IO created\n");

    // 4. 配置 MIPI DPI 面板 (用于发送像素数据)
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = RG_MIPI_DSI_DPI_CLK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .video_timing = {
            .h_size = RG_SCREEN_WIDTH, 
            .v_size = RG_SCREEN_HEIGHT, 
            .hsync_back_porch = RG_MIPI_DSI_LCD_HBP,
            .hsync_pulse_width = RG_MIPI_DSI_LCD_HSYNC,
            .hsync_front_porch = RG_MIPI_DSI_LCD_HFP,
            .vsync_back_porch = RG_MIPI_DSI_LCD_VBP,
            .vsync_pulse_width = RG_MIPI_DSI_LCD_VSYNC,
            .vsync_front_porch = RG_MIPI_DSI_LCD_VFP,
        },
        .num_fbs = 3,
        .flags.use_dma2d = true,
    };

    // 5. 创建 ST7701 面板
    st7701_vendor_config_t vendor_config = {
        .init_cmds = st7701_init_cmds,
        .init_cmds_size = sizeof(st7701_init_cmds) / sizeof(st7701_lcd_init_cmd_t),
        .flags.use_mipi_interface = 1,
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };

    esp_lcd_panel_dev_config_t lcd_dev_config = {
#ifdef RG_GPIO_LCD_RST
        .reset_gpio_num = RG_GPIO_LCD_RST,
#else
        .reset_gpio_num = -1,
#endif
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,  // RGB565
        .vendor_config = &vendor_config,
    };

    ret = esp_lcd_new_panel_st7701(mipi_dbi_io, &lcd_dev_config, &mipi_dpi_panel);
    RG_ASSERT(ret == ESP_OK, "Failed to create ST7701 panel");
    RG_LOGI("ST7701 panel created\n");

    // 6. 注册回调
    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = lcd_color_trans_done_cb,
    };
    ret = esp_lcd_dpi_panel_register_event_callbacks(mipi_dpi_panel, &cbs, NULL);
    RG_ASSERT(ret == ESP_OK, "Failed to register panel callbacks");

    // 7. 复位并初始化面板
    ret = esp_lcd_panel_reset(mipi_dpi_panel);
    RG_ASSERT(ret == ESP_OK, "Failed to reset panel");

    ret = esp_lcd_panel_init(mipi_dpi_panel);
    RG_ASSERT(ret == ESP_OK, "Failed to init panel");

    

    RG_LOGI("ST7701 MIPI DSI LCD initialized successfully\n");
}

static void lcd_deinit(void)
{
  /*   if (mipi_dpi_panel) {
        esp_lcd_panel_del(mipi_dpi_panel);
        mipi_dpi_panel = NULL;
    }

    if (mipi_dbi_io) {
        esp_lcd_panel_io_del(mipi_dbi_io);
        mipi_dbi_io = NULL;
    }

    if (mipi_dsi_bus) {
        esp_lcd_del_dsi_bus(mipi_dsi_bus);
        mipi_dsi_bus = NULL;
    }

#ifdef RG_MIPI_DSI_PHY_PWR_LDO_CHAN
    if (ldo_mipi_phy) {
        esp_ldo_release_channel(ldo_mipi_phy);
        ldo_mipi_phy = NULL;
    }
#endif

    if (transfer_done_sem) {
        vSemaphoreDelete(transfer_done_sem);
        transfer_done_sem = NULL;
    }

    if (lcd_buffers) {
        for (int i = 0; i < LCD_BUFFER_COUNT; i++) {
            if (lcd_buffer_pool[i]) {
                free(lcd_buffer_pool[i]);
                lcd_buffer_pool[i] = NULL;
            }
        }
        vQueueDelete(lcd_buffers);
        lcd_buffers = NULL;
    }
 */
    RG_LOGI("ST7701 LCD deinitialized\n");
}

static void lcd_set_backlight(float percent)
{
    float level = RG_MIN(RG_MAX(percent / 100.f, 0), 1.f);

#if defined(RG_GPIO_LCD_BCKL)
    // 如果有专门的背光引脚，使用 GPIO 控制
    gpio_set_level(RG_GPIO_LCD_BCKL, level > 0.5f ? 1 : 0);
    RG_LOGI("Backlight set to %d%%\n", (int)(100 * level));
#else
    // 否则通过面板的 disp_on_off 控制
    if (mipi_dpi_panel) {
        esp_lcd_panel_disp_on_off(mipi_dpi_panel, level > 0.1f);
    }
#endif
}

static inline uint16_t *lcd_get_buffer(size_t length)
{
    uint16_t *buffer = NULL;
    if (xQueueReceive(lcd_buffers, &buffer, pdMS_TO_TICKS(2500)) != pdTRUE) {
        RG_PANIC("Failed to get LCD buffer");
    }
    return buffer;
}

static inline void lcd_send_buffer(uint16_t *buffer, size_t length)
{
    if (length == 0) {
        // 归还未使用的缓冲区
        xQueueSend(lcd_buffers, &buffer, portMAX_DELAY);
        return;
    }

    if (!current_window.valid || !mipi_dpi_panel) {
        RG_LOGW("lcd_send_buffer called without valid window or panel\n");
        xQueueSend(lcd_buffers, &buffer, portMAX_DELAY);
        return;
    }

    // 计算实际要绘制的行数
    int pixels = length;
    int lines = pixels / current_window.width;
    
    if (lines == 0) {
        xQueueSend(lcd_buffers, &buffer, portMAX_DELAY);
        return;
    }

    // RGB565 字节序转换：rg_display.c 输出的是大端（BE），MIPI DSI 需要小端（LE）
    // 将 BE 转换为 LE：交换每个像素的高低字节
    for (int i = 0; i < pixels; i++) {
        uint16_t pixel = buffer[i];
        buffer[i] = (pixel >> 8) | (pixel << 8);
    }

    // 计算坐标（暂时不做旋转处理）
    int x_start = current_window.left;
    int y_start = current_window.top;
    int x_end = current_window.left + current_window.width - 1;
    int y_end = current_window.top + lines - 1;

    // 发送数据到面板（注意：draw_bitmap 使用 x_end+1, y_end+1）
    esp_err_t ret = esp_lcd_panel_draw_bitmap(mipi_dpi_panel, 
                                               x_start, y_start, 
                                               x_end + 1, y_end + 1, 
                                               buffer);
    
    if (ret != ESP_OK) {
        RG_LOGE("Failed to draw bitmap: %d\n", ret);
    }

    // 更新窗口的顶部位置，为下一块数据做准备
    current_window.top += lines;
    current_window.height -= lines;

    // 归还缓冲区
    xQueueSend(lcd_buffers, &buffer, portMAX_DELAY);
}

static void lcd_sync(void)
{
    // 等待传输完成
   /*  if (transfer_done_sem) {
        RG_LOGE("wait transfer done semaphore");
        xSemaphoreTake(transfer_done_sem, portMAX_DELAY);
        RG_LOGE("transfer done semaphore taken");
    } */
}

static void lcd_set_rotation(int rotation)
{
    if (!mipi_dpi_panel) {
        RG_LOGW("Panel not initialized, cannot set rotation\n");
        return;
    }

    esp_err_t ret = ESP_OK;
    
    // 根据旋转角度设置 mirror 和 swap_xy
    // 旋转通过组合镜像和 XY 交换实现
    switch (rotation) {
        case 0:  // 0° - 无旋转
            ret |= esp_lcd_panel_swap_xy(mipi_dpi_panel, false);
            ret |= esp_lcd_panel_mirror(mipi_dpi_panel, false, false);
            RG_LOGI("Screen rotation set to 0°\n");
            break;
            
        case 90:  // 90° - 顺时针旋转
            ret |= esp_lcd_panel_swap_xy(mipi_dpi_panel, true);
            ret |= esp_lcd_panel_mirror(mipi_dpi_panel, true, false);
            RG_LOGI("Screen rotation set to 90° (landscape)\n");
            break;
            
        case 180:  // 180° - 翻转
            ret |= esp_lcd_panel_swap_xy(mipi_dpi_panel, false);
            ret |= esp_lcd_panel_mirror(mipi_dpi_panel, true, true);
            RG_LOGI("Screen rotation set to 180°\n");
            break;
            
        case 270:  // 270° - 逆时针旋转
            ret |= esp_lcd_panel_swap_xy(mipi_dpi_panel, true);
            ret |= esp_lcd_panel_mirror(mipi_dpi_panel, false, true);
            RG_LOGI("Screen rotation set to 270° (landscape)\n");
            break;
            
        default:
            RG_LOGW("Invalid rotation angle: %d (must be 0, 90, 180, or 270)\n", rotation);
            return;
    }
    
    if (ret != ESP_OK) {
        RG_LOGE("Failed to set rotation: %d\n", ret);
    }
}

static void lcd_set_window(int left, int top, int width, int height)
{
    // MIPI DSI 的 draw_bitmap 需要完整坐标信息
    // 这里只保存窗口状态，实际在 lcd_send_buffer 中使用
    current_window.left = left;
    current_window.top = top;
    current_window.width = width;
    current_window.height = height;
    current_window.valid = true;

    if (left < 0 || top < 0 || (left + width) > RG_SCREEN_WIDTH || (top + height) > RG_SCREEN_HEIGHT) {
        RG_LOGW("Bad LCD window (x0=%d, y0=%d, width=%d, height=%d)\n", left, top, width, height);
    }
}

const rg_display_driver_t rg_display_driver_st7701 = {
    .name = "st7701",
};