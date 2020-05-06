/*
 * Copyright 2019, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(DATA61_GPL)
 */

#include <autoconf.h>

#include <string.h>
#include <pico_stack.h>
#include <pico_socket.h>
#include <pico_device.h>
#include <pico_addressing.h>
#include <pico_ipv4.h>
#undef PACKED
#include <sel4/sel4.h>
#include <utils/util.h>
#include <sel4utils/sel4_zf_logif.h>
#include "picoserver_client.h"
#include "picoserver_socket.h"
#include <picoserver_event.h>
#include <picoserver_peer.h>
#include <platsupport/io.h>
#include <picotcp-socket-sync.h>

/*
 * Functions exposed by the connection from the interfaces.
 */
seL4_Word pico_control_get_sender_id(void);
void pico_control_emit(unsigned int);
int pico_control_largest_badge(void);

seL4_Word pico_recv_get_sender_id(void);
void *pico_recv_buf(seL4_Word);
size_t pico_recv_buf_size(seL4_Word);
seL4_Word pico_recv_enumerate_badge(unsigned int);

seL4_Word pico_send_get_sender_id(void);
void *pico_send_buf(seL4_Word);
size_t pico_send_buf_size(seL4_Word);
seL4_Word pico_send_enumerate_badge(unsigned int);

int num_clients;
int emit_client;
/*
 * Gets the client's ID and checks that it is valid.
 */
static inline seL4_Word client_check(void)
{
    /* Client IDs start from one to avoid using the zero badge */
    seL4_Word client_id = pico_control_get_sender_id();
    ZF_LOGF_IF(client_id >= num_clients, "Client ID is greater than the number of clients registered!");
    return client_id;
}

/*
 * Performs the common operations in the various control RPC calls.
 * This includes error checking and fetching the client's socket structure.
 */
static int server_control_common(seL4_Word client_id, int socket_fd, picoserver_socket_t **ret_socket)
{
    if (socket_fd < 0) {
        return -1;
    }

    *ret_socket = client_get_socket(client_id, socket_fd);
    if (*ret_socket == NULL) {
        return -1;
    }

    return 0;
}

/*
 * Performs the common operations between the send and receive RPC calls.
 * These include error checking, fetching the client's bookkeeping structures,
 * their sockets etc.
 */
static int server_communication_common(seL4_Word client_id, int socket_fd, int len, int buffer_offset,
                                       size_t buffer_size, picoserver_socket_t **ret_socket,
                                       void **ret_buffer)
{
    ZF_LOGF_IF(ret_socket == NULL || ret_buffer == NULL, "Passed in NULL for ret_socket or ret_buffer");

    if (socket_fd < 0 || len < 0 || buffer_offset < 0) {
        return -1;
    }

    if ((buffer_offset + len) > buffer_size) {
        /* Make sure we don't overflow the buffer */
        return -1;
    }

    *ret_socket = client_get_socket(client_id, socket_fd);
    if (*ret_socket == NULL) {
        return -1;
    }

    *ret_buffer += buffer_offset;

    return 0;
}

static void socket_cb(uint16_t ev, struct pico_socket *s)
{
    /* ZF_LOGE("\033[32mpico_socket addr = %x, ev = %d\033[0m", s, ev); */

    /* Find the picoserver_socket struct that houses the pico_socket */
    picoserver_socket_t *client_socket = client_get_socket_by_addr(s);

    if (client_socket == NULL && s != NULL) {
        /*
         * For some reason, if pico_socket_listen is called to listen on
         * a socket and when a client connects to the socket, PicoTCP will
         * allocate another socket structure that will process callbacks even
         * though we have not called pico_socket_accept. This results in a
         * situation where we try to retrieve a socket from our hash table
         * without having registed it in the first place. The solution now is
         * to just ignore it and this shouldn't be a problem as PicoTCP does
         * not allow interactions without accepting the client's connection.
         *
         * The dangling callback may also happen when the client calls
         * pico_control_close instead of pico_control_shutdown +
         * pico_control_close.
         */
        return;
    }

    seL4_Word client_id = client_socket->client_id;
    int ret = client_put_event(client_id, client_socket->socket_fd, ev);
    ZF_LOGF_IF(ret == -1, "Failed to set the event flags for client %u's socket %d",
               client_id + 1, client_socket->socket_fd);

    emit_client = 1;

}

int pico_control_open(bool is_udp)
{
    seL4_Word client_id = client_check();
    picoserver_socket_t *new_socket = calloc(1, sizeof(picoserver_socket_t));
    if (new_socket == NULL) {
        ZF_LOGE("Failed to malloc memory for the picoserver struct");
        return -1;
    }

    new_socket->client_id = client_id;
    uint16_t protocol = (is_udp) ? PICO_PROTO_UDP : PICO_PROTO_TCP;
    new_socket->socket = pico_socket_open(PICO_PROTO_IPV4, protocol, &socket_cb);
    if (new_socket->socket == NULL) {
        ZF_LOGE("Failed to open a new socket through picotcp");
        free(new_socket);
        return -1;
    }

    int ret = client_put_socket(client_id, new_socket);
    if (ret == -1) {
        ZF_LOGE("Failed to put the socket into the client's hash table");
        pico_socket_close(new_socket->socket);
        free(new_socket);
        return -1;
    }
    new_socket->socket_fd = ret;

    return ret;
}

int pico_control_bind(int socket_fd, uint32_t local_addr, uint16_t port)
{
    seL4_Word client_id = client_check();


    picoserver_socket_t *client_socket = NULL;

    int ret = server_control_common(client_id, socket_fd, &client_socket);
    if (ret) {
        return -1;
    }

    port = short_be(port);

    ret = pico_socket_bind(client_socket->socket, &local_addr, &port);
    return ret;
}

int pico_control_connect(int socket_fd, uint32_t server_addr, uint16_t port)
{
    seL4_Word client_id = client_check();

    picoserver_socket_t *client_socket = NULL;

    int ret = server_control_common(client_id, socket_fd, &client_socket);
    if (ret) {
        return -1;
    }

    port = short_be(port);

    ret = pico_socket_connect(client_socket->socket, &server_addr, port);
    return ret;
}

int pico_control_listen(int socket_fd, int backlog)
{
    seL4_Word client_id = client_check();

    picoserver_socket_t *client_socket = NULL;

    int ret = server_control_common(client_id, socket_fd, &client_socket);
    if (ret) {
        return -1;
    }

    ret = pico_socket_listen(client_socket->socket, backlog);
    return ret;
}

picoserver_peer_t pico_control_accept(int socket_fd)
{
    seL4_Word client_id = client_check();

    picoserver_peer_t peer = {0};


    picoserver_socket_t *client_socket = NULL;

    int ret = server_control_common(client_id, socket_fd, &client_socket);
    if (ret) {
        peer.result = -1;
        return peer;
    }

    uint32_t peer_addr;
    uint16_t remote_port;

    struct pico_socket *socket = pico_socket_accept(client_socket->socket, &peer_addr, &remote_port);
    if (socket == NULL) {
        peer.result = -1;
        return peer;
    }

    picoserver_socket_t *new_socket = calloc(1, sizeof(picoserver_socket_t));
    if (new_socket == NULL) {
        peer.result = -1;
        pico_socket_close(socket);
        return peer;
    }

    new_socket->client_id = client_id;
    new_socket->socket = socket;

    ret = client_put_socket(client_id, new_socket);
    if (ret == -1) {
        peer.result = -1;
        pico_socket_close(socket);
        free(new_socket);
        return peer;
    }
    new_socket->socket_fd = ret;

    peer.result = 0;
    peer.socket = ret;
    peer.peer_addr = peer_addr;
    peer.peer_port = remote_port;

    return peer;
}

int pico_control_shutdown(int socket_fd, int mode)
{
    seL4_Word client_id = client_check();


    picoserver_socket_t *client_socket = NULL;

    int ret = server_control_common(client_id, socket_fd, &client_socket);
    if (ret) {
        return -1;
    }

    ret = pico_socket_shutdown(client_socket->socket, mode);
    return ret;
}

int pico_control_close(int socket_fd)
{
    seL4_Word client_id = client_check();


    picoserver_socket_t *client_socket = NULL;

    int ret = server_control_common(client_id, socket_fd, &client_socket);
    if (ret) {
        return -1;
    }

    ret = client_delete_socket(client_id, socket_fd);
    return ret;
}

picoserver_event_t pico_control_event_poll(void)
{
    seL4_Word client_id = client_check();


    /* Retrieve the client's outstanding events */
    picoserver_event_t event = {0};
    client_get_event(client_id, &event);

    return event;
}

int pico_control_get_ipv4(uint32_t *addr)
{
    struct pico_device *dev = pico_get_device("eth0");
    if (dev == NULL) {
        return -1;
    }
    *addr = pico_ipv4_link_by_dev(dev)->address.addr;

    return 0;
}

int pico_send_write(int socket_fd, int len, int buffer_offset)
{
    seL4_Word client_id = client_check();
    /*
     * client_id needs to be incremented here as the CAmkES generated interfaces are one off
     * from ours
     */

    size_t buffer_size = pico_send_buf_size(pico_send_enumerate_badge(client_id));
    void *client_buf = pico_send_buf(pico_send_enumerate_badge(client_id));
    picoserver_socket_t *client_socket = NULL;


    int ret = server_communication_common(client_id, socket_fd, len, buffer_offset,
                                          buffer_size, &client_socket, &client_buf);
    if (ret) {
        return -1;
    }

    ret = pico_socket_write(client_socket->socket, client_buf, len);
    return ret;
}

int pico_send_send(int socket_fd, int len, int buffer_offset)
{
    seL4_Word client_id = client_check();
    /*
     * client_id needs to be incremented here as the CAmkES generated interfaces are one off
     * from ours
     */
    size_t buffer_size = pico_send_buf_size(pico_send_enumerate_badge(client_id));
    void *client_buf = pico_send_buf(pico_send_enumerate_badge(client_id));
    picoserver_socket_t *client_socket = NULL;


    int ret = server_communication_common(client_id, socket_fd, len, buffer_offset,
                                          buffer_size, &client_socket, &client_buf);
    if (ret) {
        return -1;
    }

    ret = pico_socket_send(client_socket->socket, client_buf, len);
    return ret;
}

int pico_send_sendto(int socket_fd, int len, int buffer_offset, uint32_t dst_addr, uint16_t remote_port)
{
    seL4_Word client_id = client_check();
    /*
     * client_id needs to be incremented here as the CAmkES generated interfaces are one off
     * from ours
     */
    size_t buffer_size = pico_send_buf_size(pico_send_enumerate_badge(client_id));
    void *client_buf = pico_send_buf(pico_send_enumerate_badge(client_id));
    picoserver_socket_t *client_socket = NULL;


    int ret = server_communication_common(client_id, socket_fd, len, buffer_offset,
                                          buffer_size, &client_socket, &client_buf);
    if (ret) {
        return -1;
    }

    remote_port = short_be(remote_port);

    ret = pico_socket_sendto(client_socket->socket, client_buf, len, &dst_addr, remote_port);
    return ret;
}

int pico_recv_read(int socket_fd, int len, int buffer_offset)
{
    seL4_Word client_id = client_check();
    /*
     * client_id needs to be incremented here as the CAmkES generated interfaces are one off
     * from ours
     */
    size_t buffer_size = pico_recv_buf_size(pico_recv_enumerate_badge(client_id));
    void *client_buf = pico_recv_buf(pico_recv_enumerate_badge(client_id));
    picoserver_socket_t *client_socket = NULL;


    int ret = server_communication_common(client_id, socket_fd, len, buffer_offset,
                                          buffer_size, &client_socket, &client_buf);
    if (ret) {
        return -1;
    }

    ret = pico_socket_read(client_socket->socket, client_buf, len);
    return ret;
}

int pico_recv_recv(int socket_fd, int len, int buffer_offset)
{
    seL4_Word client_id = client_check();
    /*
     * client_id needs to be incremented here as the CAmkES generated interfaces are one off
     * from ours
     */
    size_t buffer_size = pico_recv_buf_size(pico_recv_enumerate_badge(client_id));
    void *client_buf = pico_recv_buf(pico_recv_enumerate_badge(client_id));
    picoserver_socket_t *client_socket = NULL;


    int ret = server_communication_common(client_id, socket_fd, len, buffer_offset,
                                          buffer_size, &client_socket, &client_buf);
    if (ret) {
        return -1;
    }

    ret = pico_socket_recv(client_socket->socket, client_buf, len);

    return ret;
}

int pico_recv_recvfrom(int socket_fd, int len, int buffer_offset, uint32_t *src_addr, uint16_t *remote_port)
{
    seL4_Word client_id = client_check();
    /*
     * client_id needs to be incremented here as the CAmkES generated interfaces are one off
     * from ours
     */
    size_t buffer_size = pico_recv_buf_size(pico_recv_enumerate_badge(client_id));
    void *client_buf = pico_recv_buf(pico_recv_enumerate_badge(client_id));
    picoserver_socket_t *client_socket = NULL;

    int ret = server_communication_common(client_id, socket_fd, len, buffer_offset,
                                          buffer_size, &client_socket, &client_buf);
    if (ret) {
        return -1;
    }

    ret = pico_socket_recvfrom(client_socket->socket, client_buf, len, src_addr, remote_port);

    /* Reverse the big endian port number */
    *remote_port = short_be(*remote_port);
    return ret;
}

static void notify_client(UNUSED seL4_Word badge, void *cookie)
{
    if (emit_client) {
        pico_control_emit(1);
        emit_client = 0;
    }
}

int picotcp_socket_sync_server_init(ps_io_ops_t *io_ops, int num_clients_,
                                    register_callback_handler_fn_t callback_handler)
{
    num_clients = num_clients_;
    picoserver_clients_init(num_clients);
    callback_handler(0, notify_client, NULL);

    return 0;
}
