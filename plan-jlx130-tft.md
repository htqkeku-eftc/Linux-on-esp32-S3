# JLX130-026 TFT 屏幕适配实施计划

## 1. 目标

将 Paperboard 显示链路从 ED097TC2 e-paper（1200×825，并行 LCD 外设 + 1bpp 差分）改为 JLX130-026（1.3寸 240×240 RGB565 彩色 TFT，ST7789V，4 线 SPI）。

## 2. 硬件接线（默认 GPIO，已确认）

| JLX130-026 引脚 | ESP32-S3 GPIO | 说明 |
|---|---|---|
| SCK | 6 | SPI 时钟 |
| SDA (MOSI) | 7 | SPI 数据 |
| CS | 5 | SPI 片选（低有效） |
| RS / A0 | 8 | 命令/数据选择（低=命令，高=数据） |
| RST | 9 | 复位（低有效） |
| LEDA（背光） | 10 | 背光（高有效，板载限流电阻） |
| VDD | 3.3V | 电源 |
| VSS | GND | 地 |

## 3. 终端布局

240×240 像素，使用 8×8 字体，行高 12px（8px 字形 + 4px 行间）：
- 列数 = 240/8 = **30**
- 行数 = 240/12 = **20**

## 4. 文件变更清单

### 4.1 新增文件

**`paperboard/linux/tft_driver.c` + `tft_driver.h`** — 固件侧 ST7789V SPI 驱动
- `spi_device_handle_t` + 5 个 GPIO（CS/SCK/SDA/RS/RST/LEDA）
- `tft_init()`：RST 脉冲 → SWRESET(0x01) → SLPOUT(0x11) → 120ms 延时 → COLMOD(0x3A=0x05 RGB565) → MADCTL(0x36=0x00) → INVON(0x21) → DISPON(0x29)
- `tft_clear(uint16_t color)`：CASET(0,239)+RASET(0,239)+RAMWR 全帧
- `tft_draw_rect(x,y,w,h,const uint16_t* px)`：设窗口后推像素
- `tft_rgb565(r,g,b)` 颜色宏
- `tft_backlight_on()`

**`paperboard/linux/tft_console.c`** — 替换 `pb_console.c` 的角色
- 240×240×2 = 115200 字节帧缓冲（PSRAM）
- `PbTerminal current, shown`（复用 ANSI 解析器）
- `raster_rgb565(PbTerminal*, uint16_t* fb)`：从 8×8 字形光栅化为 RGB565
- `console_task`：收 chunk → `pb_feed` → 节流 100ms → 对比 shown/current 脏行 → 重绘脏行到帧缓冲 → `tft_draw_rect` 推送
- `pb_console_init()`：初始化 SPI/TFT/帧缓冲/任务
- `pb_console_write()`：入队（接口不变）

### 4.2 修改文件

**`paperboard/common/terminal.h`**
- `PB_COLS` 75→30，`PB_ROWS` 34→20，`PB_WIDTH` 1200→240，`PB_HEIGHT` 825→240

**`paperboard/common/terminal.c`**
- `pb_raster_line`（e-paper 专用 1bpp 差分）改为 `raster_rgb565`（RGB565 全帧）
- `pb_dirty_lines` 保留（脏行检测逻辑通用）
- `glyph()` 函数保留但改为 RGB565 像素生成

**`paperboard/linux/epd-shell.c`**
- `winsize` 34×75 @1200×825 → 20×30 @240×240
- `COLUMNS=30`, `LINES=20`

**`paperboard/linux/pb_console.c`**
- 保留文件（headless 构建仍需要它通过 `package-final.py` 断言），但 `pb_console_init` 分支改为调用 `tft_console` 的 init
- 或：直接重写 `pb_console.c` 为 `tft_console.c` 的内容，删除 headless 标记（若 `package-final.py` 断言需同步更新）

### 4.3 构建系统

**`new-files/toplevel/apply-local-changes.sh`**
- 新增拷贝 `tft_driver.c/.h`、`tft_console.c` 到 `main/`
- 删除或注释掉 `epdiy` 拷贝（不再需要 e-paper 渲染器）

**`patches/02-firmware-network-adapter.patch`**
- CMakeLists：加 `tft_driver.c`、`tft_console.c`，加 `SPI_MASTER` 组件依赖
- 可选：移除 `epdiy` 组件依赖

**`build/package-final.py`**（第93行）
- 更新断言：headless 标记改为新标记，或保留 headless 路径

## 5. 实施顺序

1. 写 `tft_driver.c/h`（ST7789V SPI 驱动）
2. 写 `tft_console.c`（帧缓冲 + 终端渲染 + 控制台任务）
3. 改 `terminal.h`（尺寸常量）+ `terminal.c`（raster 函数）
4. 改 `epd-shell.c`（winsize/环境变量）
5. 改 `pb_console.c`（或重命名为 `tft_console.c`）
6. 改 `apply-local-changes.sh`（拷贝新文件）
7. 改 `patches/02-firmware-network-adapter.patch`（CMakeLists 加源文件）
8. 改 `build/package-final.py`（断言标记）
9. 验证编译
