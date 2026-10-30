/**
 * @file rbx.h
 * @brief rbx-protocol: Rabbit rally protocols for ESP-IDF, client and server sides.
 *
 * | Header               | Role                                                              |
 * |----------------------|-------------------------------------------------------------------|
 * | rbx_frame.h          | portable framing / encoders / parsers (no ESP-IDF)                |
 * | rbx_box_server.h     | act as a RabbitBox / Rabbit Connect (TCP and/or UDP 11912)        |
 * | rbx_box_client.h     | act as Rabbit 2 towards a box or Rabbit Connect                   |
 * | rbx_pilot_client.h   | act as a pilot screen of a main Rabbit 2 phone (UDP 7532)         |
 * | rbx_pilot_server.h   | act as the main app serving pilot screens (UDP 7532)              |
 * | rbx_link.h           | box server + automatic pilot client to whoever connects           |
 * | rb.h                 | legacy rbx-protocol-server API (rb_start / rb_send_frame)         |
 */
#ifndef RBX_H
#define RBX_H

#include "rbx_types.h"
#include "rbx_box_client.h"
#include "rbx_box_server.h"
#include "rbx_link.h"
#include "rbx_pilot_client.h"
#include "rbx_pilot_server.h"

#endif /* RBX_H */
