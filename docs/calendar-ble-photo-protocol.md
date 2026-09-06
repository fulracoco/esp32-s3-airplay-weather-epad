# Calendar photo BLE upload

The weather EPD build advertises a NimBLE service with UUID
`62750001-d828-918d-fb46-b6c11c675aec`. Its writable and notifying
characteristic is `62750002-d828-918d-fb46-b6c11c675aec`.

Photos are raw 1-bit, MSB-first bitmaps, 360 x 460 pixels, exactly 20,700
bytes. The service accepts writes up to the negotiated ATT payload size. Use
write-without-response for `DATA` packets, but keep the offset in every packet
so a sender can retry a packet safely.

Commands are little-endian:

| Command | Payload | Meaning |
| --- | --- | --- |
| `01` | `slot:u8 size:u32 crc32:u32` | Start a new upload. `size` must be 20700. |
| `02` | `offset:u32 bytes...` | Append data at the exact next offset. |
| `03` | none | Verify length and CRC, then atomically commit. |
| `04` | none | Abort and delete the temporary upload. |
| `10` | none | Request a presence bit mask notification. |

Notifications are two bytes: `status`, `detail`. A zero status acknowledges
`BEGIN`, `COMMIT`, or `ABORT`; status `0x10` carries the six-slot presence mask
in `detail`. Non-zero status values indicate a rejected command.

The device stores committed files as `/spiffs/calendar/photo00.bin` through
`photo05.bin`. The calendar page advances to the next uploaded slot on each
refresh and falls back to the built-in photo when no uploaded slot exists.
