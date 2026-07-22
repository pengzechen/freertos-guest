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
static QUEUE_ALIGN uint8_t tx_buf[128];

static int virtio_net_present;
static volatile uint32_t virtio_net_irq_count;

static const uint8_t guest_mac[6] = { 0x02, 0x58, 0x4b, 0x09, 0x0a, 0x0b };
static const uint8_t guest_ip[4] = { 10, 0, 2, 16 };
static const uint8_t host_ip[4] = { 10, 0, 2, 2 };

static void put_be16(uint8_t *p, uint16_t val)
{
    p[0] = (uint8_t)(val >> 8);
    p[1] = (uint8_t)val;
}

static uint16_t checksum16(const uint8_t *buf, uint32_t len)
{
    uint32_t sum = 0;

    while (len > 1) {
        sum += ((uint16_t)buf[0] << 8) | buf[1];
        buf += 2;
        len -= 2;
    }
    if (len != 0)
        sum += (uint16_t)buf[0] << 8;

    while ((sum >> 16) != 0)
        sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t udp_checksum(const uint8_t *ip, const uint8_t *udp, uint16_t udp_len)
{
    uint32_t sum = 0;

    for (uint32_t i = 12; i < 20; i += 2)
        sum += ((uint16_t)ip[i] << 8) | ip[i + 1];
    sum += 17U;
    sum += udp_len;

    for (uint32_t i = 0; i + 1 < udp_len; i += 2)
        sum += ((uint16_t)udp[i] << 8) | udp[i + 1];
    if ((udp_len & 1U) != 0)
        sum += (uint16_t)udp[udp_len - 1] << 8;

    while ((sum >> 16) != 0)
        sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t get_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t build_udp_test_frame(void)
{
    static const uint8_t payload[] = "hello from freertos virtio-net\n";
    const uint16_t payload_len = sizeof(payload) - 1;
    const uint16_t udp_len = 8U + payload_len;
    const uint16_t ip_len = 20U + udp_len;
    uint8_t *eth = tx_buf;
    uint8_t *ip = tx_buf + 14;
    uint8_t *udp = ip + 20;

    for (uint32_t i = 0; i < sizeof(tx_buf); i++)
        tx_buf[i] = 0;

    for (uint32_t i = 0; i < 6; i++)
        eth[i] = 0xff;
    for (uint32_t i = 0; i < 6; i++)
        eth[6 + i] = guest_mac[i];
    eth[12] = 0x08;
    eth[13] = 0x00;

    ip[0] = 0x45;
    ip[1] = 0x00;
    put_be16(ip + 2, ip_len);
    put_be16(ip + 4, 0x584b);
    put_be16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = 17;
    for (uint32_t i = 0; i < 4; i++)
        ip[12 + i] = guest_ip[i];
    for (uint32_t i = 0; i < 4; i++)
        ip[16 + i] = host_ip[i];
    put_be16(ip + 10, checksum16(ip, 20));

    put_be16(udp + 0, 5555);
    put_be16(udp + 2, 5555);
    put_be16(udp + 4, udp_len);
    for (uint32_t i = 0; i < payload_len; i++)
        udp[8 + i] = payload[i];
    put_be16(udp + 6, udp_checksum(ip, udp, udp_len));

    return 14U + ip_len;
}

static const uint8_t *rx_udp_payload(const uint8_t *frame, uint32_t frame_len, uint32_t *payload_len)
{
    if (frame_len < 42U || frame[12] != 0x08 || frame[13] != 0x00)
        return 0;

    const uint8_t *ip = frame + 14;
    if ((ip[0] >> 4) != 4 || ip[9] != 17)
        return 0;

    uint32_t ip_header_len = (ip[0] & 0x0fU) * 4U;
    uint32_t ip_total_len = get_be16(ip + 2);
    if (ip_header_len < 20U || ip_total_len < ip_header_len + 8U || frame_len < 14U + ip_total_len)
        return 0;

    const uint8_t *udp = ip + ip_header_len;
    uint32_t udp_len = get_be16(udp + 4);
    if (udp_len < 8U || udp_len > ip_total_len - ip_header_len)
        return 0;
    if (get_be16(udp + 2) != 5555U)
        return 0;

    *payload_len = udp_len - 8U;
    return udp + 8;
}

static int payload_is_ok(const uint8_t *payload, uint32_t len)
{
    return len >= 2U && payload[0] == 'o' && payload[1] == 'k';
}

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

int virtio_net_test(void)
{
    if (!virtio_net_present) {
        uart_puts("[virtio-net] skip notify test\n");
        return 0;
    }

    for (uint32_t i = 0; i < sizeof(rx_buf); i++)
        rx_buf[i] = 0;
    uint32_t tx_len = build_udp_test_frame();

    rx_desc[0].addr = (uint64_t)rx_buf;
    rx_desc[0].len = sizeof(rx_buf);
    rx_desc[0].flags = VRING_DESC_F_WRITE;
    rx_avail.ring[0] = 0;
    __asm volatile ("dmb ishst" ::: "memory");
    rx_avail.idx = 1;

    tx_desc[0].addr = (uint64_t)tx_buf;
    tx_desc[0].len = tx_len;
    tx_desc[0].flags = 0;
    tx_avail.ring[0] = 0;
    __asm volatile ("dmb ishst" ::: "memory");
    tx_avail.idx = 1;

    uart_printf("[virtio-net] submit udp 10.0.2.16:5555 -> 10.0.2.2:5555 rx_avail.idx=%u tx_avail.idx=%u tx_desc.addr=%lx tx_len=%u\n",
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

    uart_puts("[virtio-net] waiting for host reply 'ok'\n");
    uint32_t attempt = 0;
    for (;;) {
        for (uint32_t i = 0; i < sizeof(rx_buf); i++)
            rx_buf[i] = 0;

        rx_desc[0].addr = (uint64_t)rx_buf;
        rx_desc[0].len = sizeof(rx_buf);
        rx_desc[0].flags = VRING_DESC_F_WRITE;
        rx_avail.ring[rx_avail.idx % QUEUE_SIZE] = 0;
        __asm volatile ("dmb ishst" ::: "memory");
        uint16_t next_used = rx_used.idx + 1;
        rx_avail.idx++;
        REG32(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;

        for (uint32_t i = 0; i < 1000000U && rx_used.idx != next_used; i++)
            __asm volatile ("nop");
        __asm volatile ("dmb ishld" ::: "memory");

        if (rx_used.idx == next_used) {
            uint32_t payload_len = 0;
            const uint8_t *payload = rx_udp_payload(rx_buf, rx_used.ring[(next_used - 1U) % QUEUE_SIZE].len, &payload_len);
            if (payload != 0) {
                uart_printf("[virtio-net] host payload len=%u first=%x:%x\n",
                            payload_len, payload_len > 0 ? payload[0] : 0,
                            payload_len > 1 ? payload[1] : 0);
                if (payload_is_ok(payload, payload_len))
                    return 1;
            }
        }

        attempt++;
        if ((attempt % 1000U) == 0)
            uart_puts("[virtio-net] still waiting for host reply 'ok'\n");
    }
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
