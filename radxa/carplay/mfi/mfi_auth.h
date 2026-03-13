#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * MFi Authentication Coprocessor Driver
 *
 * Talks to Apple MFi auth chip (MFI341S2164 or similar) over I2C.
 * The chip handles RSA signing internally — we never see the private key.
 *
 * Wiring (QFN-20):
 *   Pin 13 (SDA) → Radxa Cubie A7Z Phys Pin 3 (TWI7_SDA / PJ23)
 *   Pin 12 (SCL) → Radxa Cubie A7Z Phys Pin 5 (TWI7_SCL / PJ22)
 *   Pin 4  (VCC) → 3.3V
 *   Pin 11 (VSS) → GND
 *   Pin 5  (nRESET) → 3.3V (HIGH = normal)
 *   Pin 2  (MODE1) → 3.3V (HIGH = I2C mode)
 *
 * I2C bus: /dev/i2c-7 on Radxa Cubie A7Z (TWI7, enable via rsetup overlay)
 * I2C address: 0x10 (verify with i2cdetect -y 7)
 */

#define MFI_I2C_BUS      "/dev/i2c-7"
#define MFI_I2C_ADDR     0x10

/* MFi register map (approximate — varies slightly by chip version) */
#define MFI_REG_DEVICE_VERSION   0x00
#define MFI_REG_AUTH_REVISION    0x01
#define MFI_REG_AUTH_PROTO_MAJOR 0x02
#define MFI_REG_AUTH_PROTO_MINOR 0x03
#define MFI_REG_DEVICE_ID        0x04
#define MFI_REG_ERROR_CODE       0x05
#define MFI_REG_AUTH_CTRL        0x10
#define MFI_REG_CHALLENGE_DATA   0x20
#define MFI_REG_CHALLENGE_LEN    0x21
#define MFI_REG_SIGNATURE_DATA   0x30
#define MFI_REG_SIGNATURE_LEN    0x31
#define MFI_REG_CERTIFICATE      0x50
#define MFI_REG_CERTIFICATE_LEN  0x51

/* Auth control register commands */
#define MFI_CTRL_START_SIGN      0x01

typedef struct {
    int i2c_fd;
    uint8_t addr;
} mfi_device_t;

/**
 * Open I2C connection to MFi chip.
 * Returns 0 on success, -1 on failure.
 */
int mfi_open(mfi_device_t *dev, const char *i2c_bus, uint8_t addr);

/**
 * Read the MFi chip device version and verify communication.
 * Returns version byte, or -1 on failure.
 */
int mfi_get_version(mfi_device_t *dev);

/**
 * Read the MFi certificate (X.509 DER format).
 * cert_buf must be at least 1024 bytes.
 * Returns actual certificate length, or -1 on failure.
 */
int mfi_get_certificate(mfi_device_t *dev, uint8_t *cert_buf, size_t buf_size);

/**
 * Sign a challenge from the iPhone.
 * iPhone sends 128-byte challenge during iAP2 auth.
 * We write it to the chip, trigger signing, read back signature.
 *
 * sig_buf must be at least 256 bytes.
 * Returns actual signature length, or -1 on failure.
 */
int mfi_sign_challenge(mfi_device_t *dev,
                        const uint8_t *challenge, size_t challenge_len,
                        uint8_t *sig_buf, size_t sig_buf_size);

/**
 * Close I2C connection.
 */
void mfi_close(mfi_device_t *dev);
