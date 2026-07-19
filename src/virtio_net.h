#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>

void virtio_net_init(void);
void virtio_net_test(void);
int virtio_net_handle_irq(uint32_t irq);

#endif
