#include "ipc.h"

#include <cstdio>
#include <gio/gunixfdmessage.h>
#include <sys/types.h>
#include <sys/socket.h>

namespace IPC {

Host::Host() = default;

void Host::initialize(Handler& handler)
{
    m_handler = &handler;

    int sockets[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
    if (ret == -1)
        return;

    m_socket = g_socket_new_from_fd(sockets[0], nullptr);
    if (!m_socket) {
        close(sockets[0]);
        close(sockets[1]);
        return;
    }

    m_source = g_socket_create_source(m_socket, G_IO_IN, nullptr);
    g_source_set_callback(m_source, reinterpret_cast<GSourceFunc>(socketCallback), this, nullptr);
    g_source_attach(m_source, g_main_context_get_thread_default());

    m_clientFd = sockets[1];
}

void Host::deinitialize()
{
    if (m_clientFd != -1)
        close(m_clientFd);

    if (m_source)
        g_source_destroy(m_source);
    if (m_socket)
        g_object_unref(m_socket);
}

int Host::releaseClientFD()
{
    int fd = m_clientFd;
    m_clientFd = -1;
    return fd;
}

void Host::send(char* data, size_t size)
{
    g_socket_send(m_socket, data, size, nullptr, nullptr);
}

gboolean Host::socketCallback(GSocket* socket, GIOCondition condition, gpointer data)
{
    if (!(condition & G_IO_IN))
        return TRUE;

    auto& host = *static_cast<Host*>(data);

    GSocketControlMessage** messages;
    int nMessages = 0;
    char* buffer = g_new0(char, host.m_messageSize);
    GInputVector vector = { buffer, host.m_messageSize };
    gssize len = g_socket_receive_message(socket, nullptr, &vector, 1,
        &messages, &nMessages, nullptr, nullptr, nullptr);

    // If nothing is read, give up.
    if (len == -1) {
        g_free(buffer);
        return FALSE;
    }

    // Safe to assume only one FD message will arrive.
    if (nMessages == 1 && G_IS_UNIX_FD_MESSAGE(messages[0])) {
        int* fds;
        int nFds = 0;
        fds = g_unix_fd_message_steal_fds(G_UNIX_FD_MESSAGE(messages[0]), &nFds);

        host.m_handler->handleFd(fds[0]);

        g_free(fds);
    }

    // But just in case, erase any and all messages.
    if (nMessages > 0) {
        for (int i = 0; i < nMessages; ++i)
            g_object_unref(messages[i]);
        g_free(messages);

        g_free(buffer);
        return TRUE;
    }

    if (len == host.m_messageSize)
        host.m_handler->handleMessage(buffer, host.m_messageSize);

    g_free(buffer);
    return TRUE;
}

Client::Client() = default;

void Client::initialize(Handler& handler, int fd)
{
    m_handler = &handler;

    m_socket = g_socket_new_from_fd(fd, nullptr);
    if (!m_socket)
        return;

    m_source = g_socket_create_source(m_socket, G_IO_IN, nullptr);
    g_source_set_callback(m_source, reinterpret_cast<GSourceFunc>(socketCallback), this, nullptr);
    g_source_attach(m_source, g_main_context_get_thread_default());
}

void Client::deinitialize()
{
}

gboolean Client::socketCallback(GSocket* socket, GIOCondition condition, gpointer data)
{
    if (!(condition & G_IO_IN))
        return TRUE;

    auto& client = *reinterpret_cast<Client*>(data);

    char* buffer = g_new0(char, client.m_messageSize);
    gssize len = g_socket_receive(socket, buffer, client.m_messageSize, nullptr, nullptr);

    if (len == client.m_messageSize)
        client.m_handler->handleMessage(buffer, client.m_messageSize);

    g_free(buffer);
    return TRUE;
}

void Client::sendFd(int fd)
{
    GSocketControlMessage* fdMessage = g_unix_fd_message_new();
    if (!g_unix_fd_message_append_fd(G_UNIX_FD_MESSAGE(fdMessage), fd, nullptr)) {
        g_object_unref(fdMessage);
        return;
    }

    if (g_socket_send_message(m_socket, nullptr, nullptr, 0, &fdMessage, 1, 0, nullptr, nullptr) == -1) {
        g_object_unref(fdMessage);
        return;
    }

    g_object_unref(fdMessage);
}

void Client::sendMessage(char* data, size_t size)
{
    g_socket_send(m_socket, data, size, nullptr, nullptr);
}

} // namespace IPC
