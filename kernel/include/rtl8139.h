#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>
#include <stddef.h>

void rtl8139_init(void);
void rtl8139_send(const void *data, size_t len);
int rtl8139_get_mac(uint8_t mac[6]);
void rtl8139_irq_handler(void);

#endif /* RTL8139_H */
