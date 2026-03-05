#include "mfi_auth.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

static int i2c_write_reg(int fd, uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[257];
    buf[0] = reg;
    if (len > sizeof(buf) - 1) return -1;
    memcpy(buf + 1, data, len);

    if (write(fd, buf, len + 1) != (ssize_t)(len + 1)) {
        perror("mfi: i2c write");
        return -1;
    }
    return 0;
}

static int i2c_read_reg(int fd, uint8_t reg, uint8_t *data, size_t len)
{
    /* Write register address */
    if (write(fd, &reg, 1) != 1) {
        perror("mfi: i2c write reg addr");
        return -1;
    }
    /* Read data */
    if (read(fd, data, len) != (ssize_t)len) {
        perror("mfi: i2c read");
        return -1;
    }
    return 0;
}

int mfi_open(mfi_device_t *dev, const char *i2c_bus, uint8_t addr)
{
    dev->addr = addr;
    dev->i2c_fd = open(i2c_bus, O_RDWR);
    if (dev->i2c_fd < 0) {
        fprintf(stderr, "mfi: failed to open %s: %s\n", i2c_bus, strerror(errno));
        return -1;
    }

    if (ioctl(dev->i2c_fd, I2C_SLAVE, addr) < 0) {
        fprintf(stderr, "mfi: failed to set I2C address 0x%02X: %s\n",
                addr, strerror(errno));
        close(dev->i2c_fd);
        dev->i2c_fd = -1;
        return -1;
    }

    printf("mfi: opened %s addr=0x%02X\n", i2c_bus, addr);
    return 0;
}

int mfi_get_version(mfi_device_t *dev)
{
    uint8_t version;
    if (i2c_read_reg(dev->i2c_fd, MFI_REG_DEVICE_VERSION, &version, 1) < 0)
        return -1;

    printf("mfi: device version = 0x%02X\n", version);
    return version;
}

int mfi_get_certificate(mfi_device_t *dev, uint8_t *cert_buf, size_t buf_size)
{
    /* Read certificate length first */
    uint8_t len_buf[2];
    if (i2c_read_reg(dev->i2c_fd, MFI_REG_CERTIFICATE_LEN, len_buf, 2) < 0)
        return -1;

    uint16_t cert_len = (len_buf[0] << 8) | len_buf[1];
    if (cert_len == 0 || cert_len > buf_size) {
        fprintf(stderr, "mfi: invalid certificate length: %u\n", cert_len);
        return -1;
    }

    /* Read certificate in chunks (I2C has limited transaction size) */
    size_t offset = 0;
    while (offset < cert_len) {
        size_t chunk = cert_len - offset;
        if (chunk > 128) chunk = 128;

        if (i2c_read_reg(dev->i2c_fd, MFI_REG_CERTIFICATE, cert_buf + offset, chunk) < 0)
            return -1;
        offset += chunk;
    }

    printf("mfi: certificate read, %u bytes\n", cert_len);
    return cert_len;
}

int mfi_sign_challenge(mfi_device_t *dev,
                        const uint8_t *challenge, size_t challenge_len,
                        uint8_t *sig_buf, size_t sig_buf_size)
{
    /* Write challenge length */
    uint8_t len_buf[2] = {
        (uint8_t)(challenge_len >> 8),
        (uint8_t)(challenge_len & 0xFF),
    };
    if (i2c_write_reg(dev->i2c_fd, MFI_REG_CHALLENGE_LEN, len_buf, 2) < 0)
        return -1;

    /* Write challenge data in chunks */
    size_t offset = 0;
    while (offset < challenge_len) {
        size_t chunk = challenge_len - offset;
        if (chunk > 128) chunk = 128;

        if (i2c_write_reg(dev->i2c_fd, MFI_REG_CHALLENGE_DATA,
                          challenge + offset, chunk) < 0)
            return -1;
        offset += chunk;
    }

    /* Trigger signing */
    uint8_t ctrl = MFI_CTRL_START_SIGN;
    if (i2c_write_reg(dev->i2c_fd, MFI_REG_AUTH_CTRL, &ctrl, 1) < 0)
        return -1;

    /* Poll for completion (~50ms typical) */
    for (int i = 0; i < 20; i++) {
        usleep(10000); /* 10ms */

        uint8_t error;
        if (i2c_read_reg(dev->i2c_fd, MFI_REG_ERROR_CODE, &error, 1) < 0)
            continue;

        if (error == 0) {
            /* Signing complete — read signature */
            uint8_t sig_len_buf[2];
            if (i2c_read_reg(dev->i2c_fd, MFI_REG_SIGNATURE_LEN, sig_len_buf, 2) < 0)
                return -1;

            uint16_t sig_len = (sig_len_buf[0] << 8) | sig_len_buf[1];
            if (sig_len == 0 || sig_len > sig_buf_size) {
                fprintf(stderr, "mfi: invalid signature length: %u\n", sig_len);
                return -1;
            }

            offset = 0;
            while (offset < sig_len) {
                size_t chunk = sig_len - offset;
                if (chunk > 128) chunk = 128;
                if (i2c_read_reg(dev->i2c_fd, MFI_REG_SIGNATURE_DATA,
                                 sig_buf + offset, chunk) < 0)
                    return -1;
                offset += chunk;
            }

            printf("mfi: challenge signed, %u bytes\n", sig_len);
            return sig_len;
        }
    }

    fprintf(stderr, "mfi: signing timed out\n");
    return -1;
}

void mfi_close(mfi_device_t *dev)
{
    if (dev->i2c_fd >= 0) {
        close(dev->i2c_fd);
        dev->i2c_fd = -1;
    }
    printf("mfi: closed\n");
}
