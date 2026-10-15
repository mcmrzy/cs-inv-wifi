#ifndef SERIAL_ADMIN_H
#define SERIAL_ADMIN_H
void serial_admin_init(void);
/* Nonblocking USB input and transaction result handling; main loop only. */
void serial_admin_tick(void);
#endif
