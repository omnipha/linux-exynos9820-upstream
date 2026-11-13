// SPDX-License-Identifier: GPL-2.0
/*
 * mb007 one-shot USB2 MUIC routing. The only write command is
 * MAX77705 CTRL1_W(COM_USB), as used by the S10e MAX77705 Type-C driver.
 * No power-role, VBUS, PD, reset, firmware, or charger commands are issued.
 * This is a diagnostic, not a substitute for the kernel Type-C driver.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define BUS_PATH "/dev/i2c-1"
#define ADAPTER_SYSFS "/sys/class/i2c-dev/i2c-1/device"
#define CCIC_ADDR 0x25
#define R_HW_REV 0x00
#define R_FW_REV 0x01
#define R_UIC_INT 0x02
#define R_CC_STATUS0 0x0a
#define R_PD_STATUS1 0x0d
#define R_MAILBOX 0x21
#define R_MAILBOX_END 0x41
#define R_MAILBOX_REPLY 0x51
#define UIC_APCMDRES 0x80
#define OP_CTRL1_W 0x06
#define MUIC_COM_USB 0x09

static int read_file(const char *path, char *buf, size_t cap, size_t *length)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror(path); return -1; }
    ssize_t n = read(fd, buf, cap - 1);
    int saved = errno;
    close(fd);
    if (n <= 0) { errno = saved; perror(path); return -1; }
    buf[n] = '\0';
    if (length) *length = (size_t)n;
    return 0;
}

static int has_token(const char *haystack, const char *token)
{
    size_t n = strlen(token);
    const char *p = haystack;
    while ((p = strstr(p, token))) {
        if ((p == haystack || p[-1] == ' ' || p[-1] == '\n') &&
            (p[n] == '\0' || p[n] == ' ' || p[n] == '\n'))
            return 1;
        p += n;
    }
    return 0;
}

static int on_expected_device(void)
{
    char buf[4096], resolved[PATH_MAX];
    size_t n;
    if (read_file("/proc/device-tree/compatible", buf, sizeof(buf), &n) < 0)
        return 0;
    if (!memmem(buf, n, "samsung,beyond2lte", strlen("samsung,beyond2lte"))) {
        fprintf(stderr, "refused: device compatible is not beyond2lte\n");
        return 0;
    }
    if (read_file("/proc/cmdline", buf, sizeof(buf), NULL) < 0 ||
        !has_token(buf, "mb007.muic=1") ||
        (!has_token(buf, "mb007.guard=1") && !has_token(buf, "mb007.usb=1"))) {
        fprintf(stderr, "refused: MUIC plus guarded or persistent USB flags required\n");
        return 0;
    }
    if (!realpath(ADAPTER_SYSFS, resolved)) {
        perror("i2c adapter realpath"); return 0;
    }
    if (!strstr(resolved, "/15d60000.i2c/")) {
        fprintf(stderr, "refused: wrong i2c adapter: %s\n", resolved); return 0;
    }
    if (access("/sys/bus/i2c/devices/1-0025/driver", F_OK) == 0) {
        fprintf(stderr, "refused: CCIC has a bound kernel driver\n"); return 0;
    }
    if (read_file("/sys/kernel/config/usb_gadget/g1/UDC", buf, sizeof(buf), NULL) < 0 ||
        strcmp(buf, "10c00000.usb\n") != 0) {
        fprintf(stderr, "refused: expected NCM gadget is not bound\n"); return 0;
    }
    printf("adapter=%s gadget=10c00000.usb\n", resolved);
    return 1;
}

static int read_reg(int fd, uint8_t reg, uint8_t *value)
{
    struct i2c_msg msg[2] = {
        { .addr = CCIC_ADDR, .flags = 0, .len = 1, .buf = &reg },
        { .addr = CCIC_ADDR, .flags = I2C_M_RD, .len = 1, .buf = value },
    };
    struct i2c_rdwr_ioctl_data x = { .msgs = msg, .nmsgs = 2 };
    int rc = ioctl(fd, I2C_RDWR, &x);
    if (rc != 2) { if (rc >= 0) errno = EIO; perror("i2c read"); return -1; }
    return 0;
}

static int write_fixed(int fd, const uint8_t *data, uint16_t len)
{
    struct i2c_msg msg = { .addr = CCIC_ADDR, .flags = 0, .len = len,
                           .buf = (uint8_t *)data };
    struct i2c_rdwr_ioctl_data x = { .msgs = &msg, .nmsgs = 1 };
    int rc = ioctl(fd, I2C_RDWR, &x);
    if (rc != 1) { if (rc >= 0) errno = EIO; perror("i2c write"); return -1; }
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t hw, fw, cc, pd, intr, reply;
    unsigned long funcs = 0;
    static const uint8_t select_usb[] = { R_MAILBOX, OP_CTRL1_W, MUIC_COM_USB };
    static const uint8_t execute[] = { R_MAILBOX_END, 0x00 };
    const struct timespec poll = { .tv_sec = 0, .tv_nsec = 5000000L };
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 2 || (strcmp(argv[1], "--inspect") && strcmp(argv[1], "--connect"))) {
        fprintf(stderr, "usage: %s --inspect|--connect\n", argv[0]); return 2;
    }
    if (!on_expected_device()) return 3;
    int fd = open(BUS_PATH, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(BUS_PATH); return 4; }
    if (ioctl(fd, I2C_FUNCS, &funcs) < 0 || !(funcs & I2C_FUNC_I2C)) {
        fprintf(stderr, "refused: adapter does not support raw I2C\n"); return 5;
    }
    /* This non-FORCE ioctl refuses an address occupied by a kernel driver. */
    if (ioctl(fd, I2C_SLAVE, CCIC_ADDR) < 0) { perror("CCIC address busy"); return 6; }
    if (read_reg(fd, R_HW_REV, &hw) || read_reg(fd, R_FW_REV, &fw) ||
        read_reg(fd, R_CC_STATUS0, &cc) || read_reg(fd, R_PD_STATUS1, &pd))
        return 7;
    printf("ccic hw=0x%02x fw=0x%02x cc_status0=0x%02x ccstat=%u pd_status1=0x%02x data_role=%s\n",
           hw, fw, cc, cc & 7, pd, (pd & 0x80) ? "host" : "device");
    if ((hw == 0xff && fw == 0xff) || (hw == 0 && fw == 0)) {
        fprintf(stderr, "refused: invalid CCIC identity\n"); return 8;
    }
    if (!strcmp(argv[1], "--inspect")) return 0;
    if ((cc & 7) != 1 || (pd & 0x80)) {
        fprintf(stderr, "refused: CCIC does not report sink + device data role\n"); return 9;
    }
    /* UIC_INT is read-to-clear; no kernel CCIC driver is allowed above. */
    if (read_reg(fd, R_UIC_INT, &intr)) return 10;
    printf("stale UIC_INT cleared=0x%02x\n", intr);
    if (write_fixed(fd, select_usb, sizeof(select_usb)) ||
        write_fixed(fd, execute, sizeof(execute))) return 11;
    for (int i = 0; i < 600; ++i) {
        if (read_reg(fd, R_UIC_INT, &intr)) return 12;
        if (intr & UIC_APCMDRES) {
            if (read_reg(fd, R_MAILBOX_REPLY, &reply)) return 13;
            printf("mailbox completed: int=0x%02x opcode_echo=0x%02x\n", intr, reply);
            if (reply != OP_CTRL1_W) { fprintf(stderr, "unexpected opcode echo\n"); return 14; }
            close(fd);
            return 0;
        }
        nanosleep(&poll, NULL);
    }
    fprintf(stderr, "mailbox command timed out\n");
    close(fd);
    return 15;
}
