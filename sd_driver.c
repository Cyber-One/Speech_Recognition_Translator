/* Pico SD card SPI driver for FatFs */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

/* FatFs integer type definitions (normally in integer.h, but we define here) */
#ifndef _DISKIO_H
typedef unsigned char   BYTE;
typedef unsigned short  WORD;
typedef unsigned long   DWORD;
typedef unsigned int    UINT;
typedef unsigned long   LBA_t;
#endif

#include "diskio.h"
#include "ff.h"

/* SD Card SPI pins */
#define SD_SPI spi0
#define SD_SCK 18
#define SD_MOSI 19
#define SD_MISO 16
#define SD_CS 17
#define SD_SPI_BAUD 10000000  /* 10 MHz */

/* SD Command definitions */
#define CMD0 0          /* Reset */
#define CMD1 1          /* Activate Init */
#define CMD8 8          /* Check voltage range */
#define CMD9 9          /* Read CSD */
#define CMD10 10        /* Read CID */
#define CMD12 12        /* Stop transmission */
#define CMD13 13        /* Get status */
#define CMD16 16        /* Set block length */
#define CMD17 17        /* Read single block */
#define CMD18 18        /* Read multiple blocks */
#define CMD23 23        /* Set block count for multi-read */
#define CMD24 24        /* Write single block */
#define CMD25 25        /* Write multiple blocks */
#define CMD32 32        /* Set erase start address */
#define CMD33 33        /* Set erase stop address */
#define CMD38 38        /* Erase selected blocks */
#define CMD41 41        /* Send operation condition (ACMD) */
#define CMD55 55        /* Prefix for app command */
#define CMD58 58        /* Read OCR */
#define CMD59 59        /* CRC on/off */

static uint8_t sd_initialized = 0;
static uint8_t sd_card_type = 0;  /* 0=SD1, 1=SD2, 2=SDHC/SDXC */

static void sd_cs_low(void) {
    gpio_put(SD_CS, 0);
}

static void sd_cs_high(void) {
    gpio_put(SD_CS, 1);
}

static uint8_t sd_spi_xfer(uint8_t data) {
    uint8_t rx;
    spi_write_read_blocking(SD_SPI, &data, &rx, 1);
    return rx;
}

static void sd_spi_write(const uint8_t *buf, size_t len) {
    spi_write_blocking(SD_SPI, buf, len);
}

static void sd_spi_read(uint8_t *buf, size_t len) {
    memset(buf, 0xFF, len);
    spi_write_read_blocking(SD_SPI, buf, buf, len);
}

static void sd_send_cmd(uint8_t cmd, uint32_t arg, uint8_t crc) {
    uint8_t frame[6];
    frame[0] = 0x40 | cmd;
    frame[1] = (arg >> 24) & 0xFF;
    frame[2] = (arg >> 16) & 0xFF;
    frame[3] = (arg >> 8) & 0xFF;
    frame[4] = arg & 0xFF;
    frame[5] = crc | 0x01;
    sd_spi_write(frame, 6);
}

static uint8_t sd_read_response(void) {
    uint8_t resp;
    for (int i = 0; i < 10; i++) {
        resp = sd_spi_xfer(0xFF);
        if ((resp & 0x80) == 0) return resp;
    }
    return 0xFF;
}

static uint8_t sd_init(void) {
    uint8_t resp;
    uint8_t buf[4];

    gpio_init(SD_CS);
    gpio_set_dir(SD_CS, GPIO_OUT);
    gpio_put(SD_CS, 1);

    spi_init(SD_SPI, SD_SPI_BAUD);
    gpio_set_function(SD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SD_MISO, GPIO_FUNC_SPI);

    /* Send 74+ clock pulses with CS high */
    sd_cs_high();
    for (int i = 0; i < 10; i++) sd_spi_xfer(0xFF);

    /* CMD0: Reset */
    sd_cs_low();
    sd_send_cmd(CMD0, 0, 0x95);
    resp = sd_read_response();
    sd_cs_high();
    sd_spi_xfer(0xFF);

    if (resp != 0x01) return 0;

    /* CMD8: Check voltage */
    sd_cs_low();
    sd_send_cmd(CMD8, 0x1AA, 0x87);
    resp = sd_read_response();
    if ((resp & 0x04) == 0) {
        sd_spi_read(buf, 4);
    }
    sd_cs_high();
    sd_spi_xfer(0xFF);

    /* ACMD41: App send op condition */
    for (int i = 0; i < 100; i++) {
        sd_cs_low();
        sd_send_cmd(CMD55, 0, 0);
        sd_read_response();
        sd_cs_high();
        sd_spi_xfer(0xFF);

        sd_cs_low();
        sd_send_cmd(CMD41, 0x40000000, 0);
        resp = sd_read_response();
        sd_cs_high();
        sd_spi_xfer(0xFF);

        if (resp == 0) break;
    }

    if (resp != 0) return 0;

    /* CMD58: Check OCR */
    sd_cs_low();
    sd_send_cmd(CMD58, 0, 0);
    resp = sd_read_response();
    sd_spi_read(buf, 4);
    sd_card_type = (buf[0] & 0x40) ? 2 : 1;
    sd_cs_high();
    sd_spi_xfer(0xFF);

    /* CMD16: Set block length */
    if (sd_card_type == 1) {
        sd_cs_low();
        sd_send_cmd(CMD16, 512, 0);
        sd_read_response();
        sd_cs_high();
        sd_spi_xfer(0xFF);
    }

    sd_initialized = 1;
    return 1;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (sd_initialized) return 0;
    if (sd_init()) return 0;
    return STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    return sd_initialized ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (!sd_initialized) return RES_NOTRDY;

    if (sd_card_type != 2) sector *= 512;

    for (UINT i = 0; i < count; i++) {
        sd_cs_low();
        sd_send_cmd(CMD17, sector + i * 512, 0);
        if (sd_read_response() != 0) {
            sd_cs_high();
            return RES_ERROR;
        }

        uint8_t token;
        for (int j = 0; j < 100000; j++) {
            token = sd_spi_xfer(0xFF);
            if (token == 0xFE) break;
        }

        sd_spi_read(buff + i * 512, 512);
        sd_spi_xfer(0xFF);
        sd_spi_xfer(0xFF);
        sd_cs_high();
        sd_spi_xfer(0xFF);
    }

    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (!sd_initialized) return RES_NOTRDY;

    if (sd_card_type != 2) sector *= 512;

    for (UINT i = 0; i < count; i++) {
        sd_cs_low();
        sd_send_cmd(CMD24, sector + i * 512, 0);
        if (sd_read_response() != 0) {
            sd_cs_high();
            return RES_ERROR;
        }

        sd_spi_xfer(0xFE);
        sd_spi_write(buff + i * 512, 512);
        sd_spi_xfer(0xFF);
        sd_spi_xfer(0xFF);

        uint8_t resp = sd_spi_xfer(0xFF);
        if ((resp & 0x1F) != 0x05) {
            sd_cs_high();
            return RES_ERROR;
        }

        for (int j = 0; j < 100000; j++) {
            if (sd_spi_xfer(0xFF) == 0xFF) break;
        }

        sd_cs_high();
        sd_spi_xfer(0xFF);
    }

    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0) return RES_PARERR;
    if (!sd_initialized) return RES_NOTRDY;

    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = 512;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD *)buff = 1;
            return RES_OK;
        case GET_SECTOR_COUNT: {
            uint8_t buf[16];
            sd_cs_low();
            sd_send_cmd(CMD9, 0, 0);
            sd_read_response();

            uint8_t token;
            for (int i = 0; i < 100000; i++) {
                token = sd_spi_xfer(0xFF);
                if (token == 0xFE) break;
            }
            sd_spi_read(buf, 16);
            sd_cs_high();
            sd_spi_xfer(0xFF);

            uint32_t c_size = ((buf[6] & 0x03) << 10) | (buf[7] << 2) | ((buf[8] >> 6) & 0x03);
            uint8_t c_size_mult = ((buf[9] & 0x03) << 1) | ((buf[10] >> 7) & 0x01);
            uint8_t read_bl_len = buf[5] & 0x0F;
            DWORD sectors = (c_size + 1) << (c_size_mult + 2) << (read_bl_len - 9);
            *(LBA_t *)buff = sectors;
            return RES_OK;
        }
        default:
            return RES_PARERR;
    }
}

DWORD get_fattime(void) {
    return ((2024 - 1980) << 25) | (1 << 21) | (1 << 16);
}
