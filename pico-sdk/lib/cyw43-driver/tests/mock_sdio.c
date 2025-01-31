#include <assert.h>
#include "src/cyw43.h"
#include "src/cyw43_internal.h"

#define BACKPLANE_BASE_ADDRESS      (0x00000000)
#define BACKPLANE_RAM_SIZE          (512 * 1024)
#define CHIPCOMMON_BASE_ADDRESS     (0x18000000)

// BUS_FUNCTION addresses.
#define SDIOD_CCCR_IOEN             (0x02)
#define SDIOD_CCCR_IORDY            (0x03)
#define SDIOD_CCCR_BLKSIZE_0        (0x10)
#define SDIOD_CCCR_SPEED_CONTROL    (0x13)

// BACKPLANE_FUNCTION addresses.
#define SDIO_BACKPLANE_ADDRESS_LOW  (0x1000a)
#define SDIO_BACKPLANE_ADDRESS_MID  (0x1000b)
#define SDIO_BACKPLANE_ADDRESS_HIGH (0x1000c)
#define SDIO_CHIP_CLOCK_CSR         (0x1000e)

#define SDIO_FUNC_ENABLE_1          (0x02)
#define SDIO_FUNC_READY_1           (0x02)
#define SBSDIO_ALP_AVAIL            (0x40)
#define SBSDIO_HT_AVAIL             (0x80)

static uint8_t sdiod_cccr_blksize_0;
static uint32_t sdio_backplane_cur_addr = 0;
static uint8_t backplane_ram[BACKPLANE_RAM_SIZE];

void cyw43_sdio_init(void) {
    printf("cyw43_sdio_init()\n");
}

void cyw43_sdio_reinit(void) {
    printf("cyw43_sdio_reinit()\n");
}

void cyw43_sdio_deinit(void) {
    printf("cyw43_sdio_deinit()\n");
}

void cyw43_sdio_set_irq(bool enable) {
    printf("cyw43_sdio_set_irq(%u)\n", enable);
}

void cyw43_sdio_enable_high_speed_4bit(void) {
    printf("cyw43_sdio_enable_high_speed_4bit()\n");
}

int cyw43_sdio_transfer(uint32_t cmd, uint32_t arg, uint32_t *resp) {
    printf("cyw43_sdio_transfer(cmd=%u, arg=%08x)\n", cmd, arg);
    if (resp != NULL) {
        *resp = 0;
    }
    if (cmd == 52) {
        uint32_t write = (arg >> 31) & 1;
        uint32_t fn = (arg >> 28) & 0x7;
        uint32_t addr = (arg >> 9) & 0x1ffff;
        uint32_t val = arg & 0xff;
        if (fn == BUS_FUNCTION) {
            if (addr == SDIOD_CCCR_IOEN) {
                if (write == 0) {
                    *resp = SDIO_FUNC_ENABLE_1;
                }
            } else if (addr == SDIOD_CCCR_IORDY) {
                if (write == 0) {
                    *resp = SDIO_FUNC_READY_1;
                }
            } else if (addr == SDIOD_CCCR_BLKSIZE_0) {
                if (write == 0) {
                    *resp = sdiod_cccr_blksize_0;
                } else {
                    sdiod_cccr_blksize_0 = val;
                }
            } else if (addr == SDIOD_CCCR_SPEED_CONTROL) {
                if (write == 0) {
                    *resp = 1; // bus supports high-speed mode
                }
            }
        } else if (fn == BACKPLANE_FUNCTION) {
            if (addr == SDIO_BACKPLANE_ADDRESS_LOW) {
                sdio_backplane_cur_addr = (sdio_backplane_cur_addr & 0xffff00ff) | (val << 8);
            } else if (addr == SDIO_BACKPLANE_ADDRESS_MID) {
                sdio_backplane_cur_addr = (sdio_backplane_cur_addr & 0xff00ffff) | (val << 16);
            } else if (addr == SDIO_BACKPLANE_ADDRESS_HIGH) {
                sdio_backplane_cur_addr = (sdio_backplane_cur_addr & 0x00ffffff) | (val << 24);
            } else if (addr == SDIO_CHIP_CLOCK_CSR) {
                if (write == 0) {
                    *resp = SBSDIO_ALP_AVAIL | SBSDIO_HT_AVAIL;
                }
            }
        }
    }
    return 0;
}

int cyw43_sdio_transfer_cmd53(bool write, uint32_t block_size, uint32_t arg, size_t len, uint8_t *buf) {
    printf("cyw43_sdio_transfer_cmd53(write=%d, block_size=%u, arg=%08x, len=%u)\n", write, block_size, arg, len);

    // Decode parameters.
    uint32_t write_decoded = (arg >> 31) & 1;
    uint32_t fn = (arg >> 28) & 0x7;
    uint32_t block_mode = (arg >> 27) & 1;
    uint32_t addr = (arg >> 9) & 0x1ffff;
    uint32_t sz = arg & 0x1ff;

    // Validate parameters.
    assert(write == write_decoded);
    if (block_mode == 0) {
        assert(block_size == sz);
        assert(sz == len);
    } else {
        assert(block_size == 64);
        assert(sz * 64 == len);
    }

    // Ignore SBSDIO_SB_ACCESS_2_4B_FLAG.
    addr &= 0x7fff;

    if (fn == BACKPLANE_FUNCTION) {
        addr += sdio_backplane_cur_addr;
        if (addr < CHIPCOMMON_BASE_ADDRESS) {
            // Read/write backplane memory.
            assert(addr + len <= BACKPLANE_BASE_ADDRESS + BACKPLANE_RAM_SIZE);
            if (write) {
                memcpy(&backplane_ram[addr], buf, len);
            } else {
                memcpy(buf, &backplane_ram[addr], len);
            }
        }
    }

    return 0;
}
