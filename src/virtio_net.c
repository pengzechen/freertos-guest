#include "virtio_net.h"
#include "gic.h"
#include "uart.h"

#include <stdint.h>

#define VIRTIO_BASE 0x0a000000UL
#define VIRTIO_IRQ  48U

#define REG32(off) (*(volatile uint32_t *)(VIRTIO_BASE + (off)))

#define VIRTIO_MMIO_MAGIC             0x000
#define VIRTIO_MMIO_VERSION           0x004
#define VIRTIO_MMIO_DEVICE_ID         0x008
#define VIRTIO_MMIO_VENDOR_ID         0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES   0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES   0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL         0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX     0x034
#define VIRTIO_MMIO_QUEUE_NUM         0x038
#define VIRTIO_MMIO_QUEUE_READY       0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY      0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS  0x060
#define VIRTIO_MMIO_INTERRUPT_ACK     0x064
#define VIRTIO_MMIO_STATUS            0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW    0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH   0x084
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW  0x090
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW  0x0a0
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH 0x0a4
#define VIRTIO_MMIO_CONFIG            0x100

#define VIRTIO_MAGIC 0x74726976U
#define VIRTIO_VERSION_2 2U
#define VIRTIO_DEVICE_NET 1U

#define VIRTIO_STATUS_ACKNOWLEDGE 1U
#define VIRTIO_STATUS_DRIVER      2U
#define VIRTIO_STATUS_DRIVER_OK   4U
#define VIRTIO_STATUS_FEATURES_OK 8U
#define VIRTIO_STATUS_FAILED      128U

#define VIRTIO_F_VERSION_1_BIT 32U
#define VIRTIO_NET_F_MAC_BIT    5U

#define QUEUE_SIZE 8U
#define QUEUE_ALIGN __attribute__((aligned(4096)))
#define VRING_DESC_F_NEXT  1U
#define VRING_DESC_F_WRITE 2U

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QUEUE_SIZE];
};

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[QUEUE_SIZE];
};

static QUEUE_ALIGN struct vring_desc rx_desc[QUEUE_SIZE];
static QUEUE_ALIGN struct vring_avail rx_avail;
static QUEUE_ALIGN struct vring_used rx_used;
static QUEUE_ALIGN struct vring_desc tx_desc[QUEUE_SIZE];
static QUEUE_ALIGN struct vring_avail tx_avail;
static QUEUE_ALIGN struct vring_used tx_used;
static QUEUE_ALIGN uint8_t rx_buf[128];
static QUEUE_ALIGN uint8_t tx_buf[64];

static int virtio_net_present;
static volatile uint32_t virtio_net_irq_count;

static void mmio_write64(uint32_t low_off, uint32_t high_off, uint64_t val)
{
    REG32(low_off) = (uint32_t)val;
    REG32(high_off) = (uint32_t)(val >> 32);
}

static uint64_t feature_word(uint32_t sel)
{
    REG32(VIRTIO_MMIO_DEVICE_FEATURES_SEL) = sel;
    return REG32(VIRTIO_MMIO_DEVICE_FEATURES);
}

static void set_status(uint32_t bits)
{
    REG32(VIRTIO_MMIO_STATUS) = REG32(VIRTIO_MMIO_STATUS) | bits;
}

static void setup_queue(uint32_t queue, void *desc, void *avail, void *used)
{
    REG32(VIRTIO_MMIO_QUEUE_SEL) = queue;
    uint32_t max = REG32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max == 0) {
        uart_printf("[virtio-net] queue %u unavailable\n", queue);
        return;
    }

    REG32(VIRTIO_MMIO_QUEUE_NUM) = QUEUE_SIZE;
    mmio_write64(VIRTIO_MMIO_QUEUE_DESC_LOW, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint64_t)desc);
    mmio_write64(VIRTIO_MMIO_QUEUE_DRIVER_LOW, VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uint64_t)avail);
    mmio_write64(VIRTIO_MMIO_QUEUE_DEVICE_LOW, VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (uint64_t)used);
    REG32(VIRTIO_MMIO_QUEUE_READY) = 1;
}

static void virtio_net_prepare_rings(void)
{
    for (uint32_t i = 0; i < QUEUE_SIZE; i++) {
        rx_desc[i].addr = 0;
        rx_desc[i].len = 0;
        rx_desc[i].flags = 0;
        rx_desc[i].next = 0;
        tx_desc[i].addr = 0;
        tx_desc[i].len = 0;
        tx_desc[i].flags = 0;
        tx_desc[i].next = 0;
        rx_avail.ring[i] = 0;
        tx_avail.ring[i] = 0;
        rx_used.ring[i].id = 0;
        rx_used.ring[i].len = 0;
        tx_used.ring[i].id = 0;
        tx_used.ring[i].len = 0;
    }
    rx_avail.flags = 0;
    rx_avail.idx = 0;
    rx_used.flags = 0;
    rx_used.idx = 0;
    tx_avail.flags = 0;
    tx_avail.idx = 0;
    tx_used.flags = 0;
    tx_used.idx = 0;
}

void virtio_net_init(void)
{
    uint32_t magic = REG32(VIRTIO_MMIO_MAGIC);
    uint32_t version = REG32(VIRTIO_MMIO_VERSION);
    uint32_t device = REG32(VIRTIO_MMIO_DEVICE_ID);
    uint32_t vendor = REG32(VIRTIO_MMIO_VENDOR_ID);

    uart_printf("[virtio-net] probe magic=%x version=%u device=%u vendor=%x\n",
                magic, version, device, vendor);

    if (magic != VIRTIO_MAGIC || version != VIRTIO_VERSION_2 || device != VIRTIO_DEVICE_NET) {
        uart_puts("[virtio-net] not present\n");
        return;
    }

    REG32(VIRTIO_MMIO_STATUS) = 0;
    set_status(VIRTIO_STATUS_ACKNOWLEDGE);
    set_status(VIRTIO_STATUS_DRIVER);

    uint64_t features = feature_word(0) | (feature_word(1) << 32);
    uint64_t wanted = (1ULL << VIRTIO_F_VERSION_1_BIT) | (1ULL << VIRTIO_NET_F_MAC_BIT);
    uint64_t accepted = features & wanted;

    REG32(VIRTIO_MMIO_DRIVER_FEATURES_SEL) = 0;
    REG32(VIRTIO_MMIO_DRIVER_FEATURES) = (uint32_t)accepted;
    REG32(VIRTIO_MMIO_DRIVER_FEATURES_SEL) = 1;
    REG32(VIRTIO_MMIO_DRIVER_FEATURES) = (uint32_t)(accepted >> 32);
    set_status(VIRTIO_STATUS_FEATURES_OK);

    if ((REG32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0) {
        REG32(VIRTIO_MMIO_STATUS) = REG32(VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FAILED;
        uart_puts("[virtio-net] FEATURES_OK rejected\n");
        return;
    }

    virtio_net_prepare_rings();
    setup_queue(0, rx_desc, &rx_avail, &rx_used);
    setup_queue(1, tx_desc, &tx_avail, &tx_used);

    set_status(VIRTIO_STATUS_DRIVER_OK);
    gic_enable_irq(VIRTIO_IRQ);
    virtio_net_present = 1;

    uart_printf("[virtio-net] ready mac=%x:%x:%x:%x:%x:%x features=%lx\n",
                REG32(VIRTIO_MMIO_CONFIG + 0), REG32(VIRTIO_MMIO_CONFIG + 1),
                REG32(VIRTIO_MMIO_CONFIG + 2), REG32(VIRTIO_MMIO_CONFIG + 3),
                REG32(VIRTIO_MMIO_CONFIG + 4), REG32(VIRTIO_MMIO_CONFIG + 5),
                (unsigned long)accepted);
}

void virtio_net_test(void)
{
    if (!virtio_net_present) {
        uart_puts("[virtio-net] skip notify test\n");
        return;
    }

    for (uint32_t i = 0; i < sizeof(rx_buf); i++)
        rx_buf[i] = 0;
    for (uint32_t i = 0; i < sizeof(tx_buf); i++)
        tx_buf[i] = (uint8_t)i;

    tx_buf[0] = 0xff;
    tx_buf[1] = 0xff;
    tx_buf[2] = 0xff;
    tx_buf[3] = 0xff;
    tx_buf[4] = 0xff;
    tx_buf[5] = 0xff;
    tx_buf[6] = 0x02;
    tx_buf[7] = 0x58;
    tx_buf[8] = 0x4b;
    tx_buf[12] = 0x08;
    tx_buf[13] = 0x00;

    rx_desc[0].addr = (uint64_t)rx_buf;
    rx_desc[0].len = sizeof(rx_buf);
    rx_desc[0].flags = VRING_DESC_F_WRITE;
    rx_avail.ring[0] = 0;
    __asm volatile ("dmb ishst" ::: "memory");
    rx_avail.idx = 1;

    tx_desc[0].addr = (uint64_t)tx_buf;
    tx_desc[0].len = 60;
    tx_desc[0].flags = 0;
    tx_avail.ring[0] = 0;
    __asm volatile ("dmb ishst" ::: "memory");
    tx_avail.idx = 1;

    uart_printf("[virtio-net] submit rx_avail.idx=%u tx_avail.idx=%u tx_desc.addr=%lx tx_len=%u\n",
                rx_avail.idx, tx_avail.idx,
                (unsigned long)tx_desc[0].addr, tx_desc[0].len);

    virtio_net_irq_count = 0;
    REG32(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;
    REG32(VIRTIO_MMIO_QUEUE_NOTIFY) = 1;

    for (uint32_t i = 0; i < 1000000U && (tx_used.idx == 0 || rx_used.idx == 0); i++) {
        __asm volatile ("nop");
    }

    __asm volatile ("dmb ishld" ::: "memory");
    uart_printf("[virtio-net] notify irq_count=%u tx_used.idx=%u tx_len=%u rx_used.idx=%u rx_len=%u rx0=%x:%x:%x:%x\n",
                virtio_net_irq_count,
                tx_used.idx, tx_used.ring[0].len,
                rx_used.idx, rx_used.ring[0].len,
                rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);
}

int virtio_net_handle_irq(uint32_t irq)
{
    if (irq != VIRTIO_IRQ)
        return 0;

    uint32_t isr = REG32(VIRTIO_MMIO_INTERRUPT_STATUS);
    if (isr != 0)
        REG32(VIRTIO_MMIO_INTERRUPT_ACK) = isr;
    virtio_net_irq_count++;
    return 1;
}
