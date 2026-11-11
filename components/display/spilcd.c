/**
 ****************************************************************************************************
 * @file        spilcd.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       SPILCD驱动代码
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ****************************************************************************************************
 * @attention
 *
 * 实验平台:正点原子 ESP32-S3开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 *
 ****************************************************************************************************
 */

#include "spilcd.h"
#include "spilcdfont.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

 
static esp_lcd_panel_handle_t s_panel_handle;
static _spilcd_dev s_spilcddev;

/* ── Mutex for SPI bus access (used by refresh timer + QR display) ── */
static SemaphoreHandle_t s_spi_mutex;

void display_lock(void)   { if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, portMAX_DELAY); }
void display_unlock(void) { if (s_spi_mutex) xSemaphoreGive(s_spi_mutex); }

void display_draw_bitmap(int x, int y, int w, int h, const uint16_t *data) {
    display_lock();
    esp_lcd_panel_draw_bitmap(s_panel_handle, x, y, x + w, y + h, data);
    display_unlock();
}

/* LCD的宽和高定义 */
uint16_t spilcd_width  = 160;       /* 屏幕的宽度 160(横屏) */
uint16_t spilcd_height = 80;        /* 屏幕的宽度 80(横屏) */

/**
 * @brief       spilcd初始化
 * @param       无
 * @retval      ESP_OK:初始化成功
 */
esp_err_t spilcd_init(void)
{
    s_spi_mutex = xSemaphoreCreateMutex();

    gpio_config_t gpio_init_struct = {0};
    /* BL管脚 */
    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;                 /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;                       /* 配置输出模式 */
    gpio_init_struct.pin_bit_mask = 1ull << LCD_PWR_PIN;            /* 配置引脚位掩码 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_ENABLE;           /* 使能下拉 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_DISABLE;              /* 失能上拉 */
    gpio_config(&gpio_init_struct);                                 /* 引脚配置 */

    LCD_PWR(0);

    spi_bus_config_t buscfg = {
        .sclk_io_num     = LCD_SCLK_PIN,    /* 时钟引脚 */
        .mosi_io_num     = LCD_MOSI_PIN,    /* 主机输出从机输入引脚 */
        .miso_io_num     = LCD_MISO_PIN,    /* 主机输入从机输出引脚 */
        .quadwp_io_num   = -1,              /* 用于Quad模式的WP引脚,未使用时设置为-1 */
        .quadhd_io_num   = -1,              /* 用于Quad模式的HD引脚,未使用时设置为-1 */
        .max_transfer_sz = spilcd_width * spilcd_height * sizeof(uint16_t),   /* 最大传输大小(整屏(RGB565格式)) */
    };
    /* 初始化SPI总线 */
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;     /* LCD IO设备句柄 */
    /* spi配置 */
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num         = LCD_DC_PIN,          /* DC IO */
        .cs_gpio_num         = LCD_CS_PIN,          /* CS IO */
        .pclk_hz             = 20 * 1000 * 1000,    /* PCLK为20MHz */
        .lcd_cmd_bits        = 8,                   /* 命令位宽 */
        .lcd_param_bits      = 8,                   /* LCD参数位宽 */
        .spi_mode            = 0,                   /* SPI模式 */
        .trans_queue_depth   = 7,                   /* 传输队列 */
    };
    /* 将LCD设备挂载至SPI总线上 */
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    s_spilcddev.pheight = spilcd_height;  /* 高度 */
    s_spilcddev.pwidth  = spilcd_width;   /* 宽度 */

    /* LCD设备配置 */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST_PIN,                  /* RTS IO */
        .rgb_ele_order  = COLOR_RGB_ELEMENT_ORDER_BGR,  /* BGR颜色格式 */
        .bits_per_pixel = 16,                           /* 颜色深度 */
        .data_endian    = LCD_RGB_DATA_ENDIAN_BIG,      /* 大端顺序 */
    };
    /* 为ST7789创建LCD面板句柄，并指定SPI IO设备句柄 */
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel_handle));
    /* 复位LCD */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    /* 反显 */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel_handle, true));
    /* 初始化LCD句柄 */
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));
    /* 打开屏幕 */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, true));

    spilcd_display_dir(1);      /* 横屏显示 */
    
    spilcd_clear(WHITE);        /* 清屏 */
    LCD_PWR(1);
    return ESP_OK;
}

/**
 * @brief       设置屏幕方向
 * @param       dir: 0为竖屏，1为横屏
 * @retval      无
 */
void spilcd_display_dir(uint8_t dir)
{
    s_spilcddev.dir = dir;

    if (s_spilcddev.dir == 0)         /* 竖屏 */
    {
        s_spilcddev.width = s_spilcddev.pheight;
        s_spilcddev.height = s_spilcddev.pwidth;
        esp_lcd_panel_swap_xy(s_panel_handle, false);
        esp_lcd_panel_mirror(s_panel_handle, false, false);
        esp_lcd_panel_set_gap(s_panel_handle, 26, 1);
    }
    else if (s_spilcddev.dir == 1)    /* 横屏 */
    {
        s_spilcddev.width = s_spilcddev.pwidth;
        s_spilcddev.height = s_spilcddev.pheight;
        esp_lcd_panel_swap_xy(s_panel_handle, true);
        esp_lcd_panel_mirror(s_panel_handle, true, false);
        esp_lcd_panel_set_gap(s_panel_handle, 1, 26);
    }
}

/**
 * @brief       清屏
 * @param       color: 颜色值
 * @retval      无
 */
void spilcd_clear(uint16_t color)
{
    /* 以40行作为缓冲,提高速率,若出现内存不足,可以减少缓冲行数 */
    uint16_t *buffer = heap_caps_malloc(s_spilcddev.width * sizeof(uint16_t) * 40, MALLOC_CAP_DMA);
    uint16_t color_tmp = ((color & 0x00FF) << 8) | ((color & 0xFF00) >> 8);   /* 需要转换一下颜色值 */
    if (NULL == buffer)
    {
        ESP_LOGE("TAG", "Memory for bitmap is not enough");
    }
    else
    {
        for (uint32_t i = 0; i < s_spilcddev.width * 40; i++)
        {
            buffer[i] = color_tmp;
        }
        
        for (uint16_t y = 0; y < s_spilcddev.height; y+=40)
        {
            esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y, s_spilcddev.width, y + 40, buffer);
        }
    }

    heap_caps_free(buffer);
}

/**
 * @brief       在指定区域内填充单个颜色
 * @param       (sx,sy),(ex,ey):填充矩形对角坐标,区域大小为:(ex - sx + 1) * (ey - sy + 1)
 * @param       color:要填充的颜色
 * @retval      无
 */
void spilcd_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color)
{
    /* 计算填充区域宽度 */
    uint16_t width = ex -sx;
    uint16_t height = ey - sy;

    /* 分配内存 */
    uint16_t *buffer = heap_caps_malloc(width * sizeof(uint16_t), MALLOC_CAP_DMA);

    if (NULL == buffer)
    {
        ESP_LOGE("TAG", "Memory for bitmap is not enough");
    }
    else
    {
        /* 填充颜色 */
        for (uint16_t i = 0; i < width; i++)
        {
            buffer[i] = color;
        }

        /* 绘制填充区域 */
        for (uint16_t y = 0; y < height; y++)
        {
            esp_lcd_panel_draw_bitmap(s_panel_handle, sx, sy + y, sx + width, sy + y + 1, buffer);
        }

        /* 释放内存 */
        heap_caps_free(buffer);
    }
}

/**
 * @brief       绘画一个像素点
 * @param       x    : x轴坐标
 * @param       y    : y轴坐标
 * @param       color: 颜色值
 * @retval      无
 */
void spilcd_draw_point(uint16_t x, uint16_t y, uint16_t color)
{
    uint16_t color_tmp = ((color & 0x00FF) << 8) | ((color & 0xFF00) >> 8);   /* 需要转换一下颜色值 */
    esp_lcd_panel_draw_bitmap(s_panel_handle, x, y, x + 1, y + 1, &color_tmp);
}

/**
 * @brief       画线函数(直线、斜线)
 * @param       x1,y1   起点坐标
 * @param       x2,y2   终点坐标
 * @param       color 填充颜色
 * @retval      无
 */
void spilcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    uint16_t t;
    int xerr = 0, yerr = 0, delta_x, delta_y, distance;
    int incx, incy, row, col;
    delta_x = x2 - x1;
    delta_y = y2 - y1;
    row = x1;
    col = y1;

    if (delta_x > 0)
    {
        incx = 1;
    }
    else if (delta_x == 0)
    {
        incx = 0;
    }
    else
    {
        incx = -1;
        delta_x = -delta_x;
    }

    if (delta_y > 0)
    {
        incy = 1;
    }
    else if (delta_y == 0)
    {
        incy = 0;
    }
    else
    {
        incy = -1;
        delta_y = -delta_y;
    }

    if ( delta_x > delta_y)
    {
        distance = delta_x;
    }
    else
    {
        distance = delta_y;
    }

    for (t = 0; t <= distance + 1; t++)
    {
        spilcd_draw_point(row, col, color);
        xerr += delta_x;
        yerr += delta_y;

        if (xerr > distance)
        {
            xerr -= distance;
            row += incx;
        }

        if (yerr > distance)
        {
            yerr -= distance;
            col += incy;
        }
    }
}

/**
 * @brief       画水平线
 * @param       x0,y0: 起点坐标
 * @param       len  : 线长度
 * @param       color: 矩形的颜色
 * @retval      无
 */
void spilcd_draw_hline(uint16_t x, uint16_t y, uint16_t len, uint16_t color)
{
    /* 确保坐标在LCD范围内 */
    if (len == 0 || x >= s_spilcddev.width || y >= s_spilcddev.height) return;

    uint16_t ex = fmin(s_spilcddev.width - 1, x + len - 1);
    uint16_t ey = y;

    /* 填充颜色区域 */
    uint32_t width = ex - x + 1;
    uint32_t h = ey - y + 1;
    uint16_t *color_buffer = malloc(width * h * sizeof(uint16_t));

    if (color_buffer == NULL) 
    {
        return; /* 检查内存分配是否成功 */
    }

    for (uint32_t i = 0; i < width * h; i++)
    {
        color_buffer[i] = color;
    }

    esp_lcd_panel_draw_bitmap(s_panel_handle, x, y, ex + 1, ey + 1, color_buffer);
    free(color_buffer);
}

/**
 * @brief       画一个矩形
 * @param       x1,y1   起点坐标
 * @param       x2,y2   终点坐标
 * @param       color 填充颜色
 * @retval      无
 */
void spilcd_draw_rectangle(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,uint16_t color)
{
    spilcd_draw_line(x0, y0, x1, y0,color);
    spilcd_draw_line(x0, y0, x0, y1,color);
    spilcd_draw_line(x0, y1, x1, y1,color);
    spilcd_draw_line(x1, y0, x1, y1,color);
}

/**
 * @brief       画一个圆
 * @param       x0,y0   圆心坐标
 * @param       r   圆半径
 * @param       color 填充颜色
 * @retval      无
 */
void spilcd_draw_circle(uint16_t x0, uint16_t y0, uint16_t r, uint16_t color)
{
    int a, b;
    int di;
    a = 0;
    b = r;
    di = 3 - (r << 1);

    while (a <= b)
    {
        spilcd_draw_point(x0 - b, y0 - a, color);
        spilcd_draw_point(x0 + b, y0 - a, color);
        spilcd_draw_point(x0 - a, y0 + b, color);
        spilcd_draw_point(x0 - b, y0 - a, color);
        spilcd_draw_point(x0 - a, y0 - b, color);
        spilcd_draw_point(x0 + b, y0 + a, color);
        spilcd_draw_point(x0 + a, y0 - b, color);
        spilcd_draw_point(x0 + a, y0 + b, color);
        spilcd_draw_point(x0 - b, y0 + a, color);
        a++;

        if (di < 0)
        {
            di += 4 * a + 6;
        }
        else
        {
            di += 10 + 4 * (a - b);
            b--;
        }

        spilcd_draw_point(x0 + a, y0 + b, color);
    }
}

/**
 * @brief       在指定位置显示一个字符
 * @param       x,y  : 坐标
 * @param       chr  : 要显示的字符:" "--->"~"
 * @param       size : 字体大小 12/16/24/32
 * @param       mode : 叠加方式(1); 非叠加方式(0);
 * @param       color : 字符的颜色;
 * @retval      无
 */
void spilcd_show_char(uint16_t x, uint16_t y, uint8_t chr, uint8_t size, uint8_t mode, uint16_t color)
{
    uint8_t temp, t1, t;
    uint16_t y0 = y;
    uint8_t csize = 0;
    uint8_t *pfont = 0;

    csize = (size / 8 + ((size % 8) ? 1 : 0)) * (size / 2); /* 得到字体一个字符对应点阵集所占的字节数 */
    chr = chr - ' ';    /* 得到偏移后的值（ASCII字库是从空格开始取模，所以-' '就是对应字符的字库） */

    switch (size)
    {
        case 12:
            pfont = (uint8_t *)asc2_1206[chr];  /* 调用1206字体 */
            break;

        case 16:
            pfont = (uint8_t *)asc2_1608[chr];  /* 调用1608字体 */
            break;

        case 24:
            pfont = (uint8_t *)asc2_2412[chr];  /* 调用2412字体 */
            break;

        case 32:
            pfont = (uint8_t *)asc2_3216[chr];  /* 调用3216字体 */
            break;

        default:
            return ;
    }

    for (t = 0; t < csize; t++)
    {
        temp = pfont[t];    /* 获取字符的点阵数据 */

        for (t1 = 0; t1 < 8; t1++)   /* 一个字节8个点 */
        {
            if (temp & 0x80)        /* 有效点,需要显示 */
            {
                spilcd_draw_point(x, y, color);        /* 画点出来,要显示这个点 */
            }
            else if (mode == 0)     /* 无效点,不显示 */
            {
                spilcd_draw_point(x, y, BLACK); /* 画背景色,相当于这个点不显示(注意背景色由全局变量控制) */
            }

            temp <<= 1; /* 移位, 以便获取下一个位的状态 */
            y++;

            if (y >= s_spilcddev.height)return;  /* 超区域了 */

            if ((y - y0) == size)   /* 显示完一列了? */
            {
                y = y0; /* y坐标复位 */
                x++;    /* x坐标递增 */

                if (x >= s_spilcddev.width)return;   /* x坐标超区域了 */

                break;
            }
        }
    }
}

/**
 * @brief       m^n函数
 * @param       m,n: 输入参数
 * @retval      m^n次方
 */
uint32_t lcd_pow(uint8_t m, uint8_t n)
{
    uint32_t result = 1;

    while(n--) result *= m;

    return result;
}

/**
 * @brief       显示len个数字
 * @param       x,y : 起始坐标
 * @param       num : 数值(0 ~ 2^32)
 * @param       len : 显示数字的位数
 * @param       size: 选择字体 12/16/24/32
 * @retval      无
 */
void spilcd_show_num(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint16_t color)
{
    uint8_t t, temp;
    uint8_t enshow = 0;

    for (t = 0; t < len; t++)                                               /* 按总显示位数循环 */
    {
        temp = (num / lcd_pow(10, len - t - 1)) % 10;                       /* 获取对应位的数字 */

        if (enshow == 0 && t < (len - 1))                                   /* 没有使能显示,且还有位要显示 */
        {
            if (temp == 0)
            {
                spilcd_show_char(x + (size / 2)*t, y, ' ', size, 0, color);    /* 显示空格,占位 */
                continue;                                                   /* 继续下个一位 */
            }
            else
            {
                enshow = 1;                                                 /* 使能显示 */
            }

        }

        spilcd_show_char(x + (size / 2)*t, y, temp + '0', size, 0, color);     /* 显示字符 */
    }
}

/**
 * @brief       扩展显示len个数字(高位是0也显示)
 * @param       x,y : 起始坐标
 * @param       num : 数值(0 ~ 2^32)
 * @param       len : 显示数字的位数
 * @param       size: 选择字体 12/16/24/32
 * @param       mode: 显示模式
 *              [7]:0,不填充;1,填充0.
 *              [6:1]:保留
 *              [0]:0,非叠加显示;1,叠加显示.
 * @param       color : 数字的颜色;
 * @retval      无
 */
void spilcd_show_xnum(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint8_t mode, uint16_t color)
{
    uint8_t t, temp;
    uint8_t enshow = 0;

    for (t = 0; t < len; t++)                                                           /* 按总显示位数循环 */
    {
        temp = (num / lcd_pow(10, len - t - 1)) % 10;                                   /* 获取对应位的数字 */

        if (enshow == 0 && t < (len - 1))                                               /* 没有使能显示,且还有位要显示 */
        {
            if (temp == 0)
            {
                if (mode & 0X80)                                                        /* 高位需要填充0 */
                {
                    spilcd_show_char(x + (size / 2)*t, y, '0', size, mode & 0X01, color);  /* 用0占位 */
                }
                else
                {
                    spilcd_show_char(x + (size / 2)*t, y, ' ', size, mode & 0X01, color);  /* 用空格占位 */
                }
                continue;
            }
            else
            {
                enshow = 1;                                                             /* 使能显示 */
            }
        }
        spilcd_show_char(x + (size / 2)*t, y, temp + '0', size, mode & 0X01, color);
    }
}

/**
 * @brief       显示字符串
 * @param       x,y         : 起始坐标
 * @param       width,height: 区域大小
 * @param       size        : 选择字体 12/16/24/32
 * @param       p           : 字符串首地址
 * @retval      无
 */
void spilcd_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t size, char *p, uint16_t color)
{
    uint8_t x0 = x;
    width += x;
    height += y;

    while ((*p <= '~') && (*p >= ' '))   /* 判断是不是非法字符! */
    {
        if (x >= width)
        {
            x = x0;
            y += size;
        }

        if (y >= height) break;  /* 退出 */

        spilcd_show_char(x, y, *p, size, 0, color);
        x += size / 2;
        p++;
    }
}

