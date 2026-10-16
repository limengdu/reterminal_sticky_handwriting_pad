# reTerminal Sticky Handwriting Pad

English | [简体中文](README.zh-CN.md)

An offline, finger-operated handwriting notebook for Seeed Studio reTerminal Sticky. Write and sketch on the e-paper display, browse up to 20 pages, and save your notebook when the device sleeps.

## Features

- Portrait layout with the hardware buttons on the right.
- Four pen styles: round, calligraphy, pencil, and speed-sensitive brush.
- Four sizes and four ink colors: black, dark gray, light gray, and white eraser.
- A floating toolbar opened from the bottom handle or with an upward swipe.
- Per-page undo and redo, up to 50 steps per page within the shared memory budget.
- Notebook pages, pen settings, and undo history restored after sleep or a saved power cycle.
- Fast partial refresh while writing; gray ink settles into a grayscale full refresh after 1.5 seconds.
- Automatic save and deep sleep after one minute of inactivity.

![Handwriting and the floating pen toolbar on reTerminal Sticky](assets/preview.jpg)

## Hardware

- reTerminal Sticky with ESP32-S3R8, 32 MB flash, and PSRAM.
- SSD1677 e-paper display and GT911 touch controller.
- USB data cable for flashing and serial logs.

The firmware runs locally on the device. A network connection, account, and external storage are not required.

## Controls

| Control | Action |
| --- | --- |
| Touch the page | Write or erase with the selected pen |
| Page Up | Previous page |
| Page Down | Next page; create a new page at the end, up to 20 |
| AI button, short press | Save and enter deep sleep; press again to wake |
| AI button, hold for one second | Open the clear-page confirmation dialog |
| Undo / redo in the top bar | Undo or redo an edit on the current page |
| Tap the bottom handle or swipe up from the bottom edge | Open the pen toolbar |
| Swipe down, tap the toolbar grip, or tap outside | Close the toolbar |

The bottom 48 pixels are the toolbar gesture area. Start writing above this area. Before disconnecting power, press the AI button and let the device finish saving and enter sleep, indicated by the moon icon.

## Build and flash

Install PlatformIO Core and run these commands from the project root. The project pins `espressif32@6.11.0`, uses ESP-IDF, and records managed component versions in `dependencies.lock`.

```sh
pio run -e sticky-release
```

Connect the Sticky with a USB data cable, then upload and open the serial monitor:

```sh
pio run -e sticky-release -t upload
pio device monitor -b 115200
```

For additional logs, build or upload the `sticky-debug` environment.

The partition table in `partitions.csv` allocates an 8 MB application partition, a 2 MB `notes` partition, and a 4 MB `history` partition.

## Device verification

1. Build and flash `sticky-release`; check that the notebook opens with `PAGE 1/1` on a fresh device.
2. Write continuous strokes and try every pen style, size, and color from the toolbar.
3. Exercise undo, redo, and both actions in the clear-page dialog.
4. Create and switch pages with the side buttons; check each page keeps its own content and edit history.
5. Save with the AI button, wake the device, and verify pages, pen settings, and undo history. Repeat with a power cycle after saving.
6. Leave the device idle for one minute and verify automatic sleep.

## Source layout

- `src/main.cpp`: `app_main()` initializes the board and starts the notebook.
- `src/board/`: power, charger, pin mapping, and shared SPI setup.
- `src/input/`: button events and queued touch samples.
- `src/display/` and `src/ui/`: e-paper refresh, canvas, and fonts.
- `src/handwrite/handwrite_app.cpp`: pages, controls, toolbar, and application loop.
- `src/handwrite/handwrite_brush.cpp`: pen rendering.
- `src/handwrite/handwrite_storage.cpp`: saved notebook pages and pen settings.
- `src/handwrite/handwrite_history.cpp`: undo, redo, and saved edit history.
- `components/`: local touch, e-paper, and logging components.
- `boards/`: PlatformIO board definition.

At startup, `app_main()` prepares power and peripherals, then calls `handwrite_app_run()`. The application restores saved pages and history, reads touch and button events, draws changes, and refreshes the display. Before sleep it saves notebook pages followed by edit history.

## Support

Report issues at the [project issue tracker](https://github.com/limengdu/reterminal_sticky_handwriting_pad/issues).

## Firmware download

The [v0.1.0 release](https://github.com/limengdu/reterminal_sticky_handwriting_pad/releases/tag/v0.1.0) provides the hardware-tested firmware package, flash manifest, and SHA-256 checksums. It contains the original PlatformIO `sticky-release` artifacts confirmed on reTerminal Sticky.

## License

[MIT](LICENSE), copyright 2026 limengdu.
