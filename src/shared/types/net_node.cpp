#include "types/net_node.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "utils/network.hpp"

using std::stringstream;
using std::string;

namespace fr {

NetNode::NetNode(DispatchCallback dispatcher, const string& address) :
 state(State::NONE),
 mode(ReadMode::HEADER),
 message(),
 nread(0),
 nwritten(0),
 flushed(false),
 _dispatcher(dispatcher) {
    size_t pos = address.find(':');
    if (pos == string::npos) {
        ip = address;
        port = 19400;
    } else {
        ip = address.substr(0, pos);
        stringstream stream(address.substr(pos + 1));
        stream >> port;
    }
}

NetNode::NetNode(DispatchCallback dispatcher) :
 state(State::NONE),
 ip(""),
 port(0),
 mode(ReadMode::HEADER),
 message(),
 nread(0),
 nwritten(0),
 flushed(false),
 _dispatcher(dispatcher) {}

void NetNode::Receive(const char* buf, ssize_t len) {
    if (buf == nullptr || len <= 0) return;

    size_t header_size = sizeof(message.kind) + sizeof(message.size);

    const char* from = buf;
    ssize_t remaining = len;

    while (true) {
        if (mode == ReadMode::HEADER) {
            char* to = reinterpret_cast<char*>(
             reinterpret_cast<uintptr_t>(&message) +
             static_cast<uintptr_t>(nread));

            ssize_t bytes_to_go = header_size - nread;

            if (bytes_to_go <= remaining) {
                memcpy(to, from, bytes_to_go);

                remaining -= bytes_to_go;

                from = reinterpret_cast<const char*>(
                 reinterpret_cast<uintptr_t>(from) +
                 static_cast<uintptr_t>(bytes_to_go));

                nread = 0;
                if (message.size > 0) {
                    message.body = malloc(message.size);
                } else {
                    message.body = nullptr;
                    message.size = 0;
                }

                mode = ReadMode::BODY;
                continue;
            } else {
                memcpy(to, from, remaining);
                nread += remaining;
                break;
            }
        } else {
            char* to = reinterpret_cast<char*>(
             reinterpret_cast<uintptr_t>(message.body) +
             static_cast<uintptr_t>(nread));

            ssize_t bytes_to_go = message.size - nread;

            if (bytes_to_go <= remaining) {
                memcpy(to, from, bytes_to_go);

                remaining -= bytes_to_go;

                from = reinterpret_cast<const char*>(
                 reinterpret_cast<uintptr_t>(from) +
                 static_cast<uintptr_t>(bytes_to_go));

                nread = 0;
                _dispatcher(this);

                mode = ReadMode::HEADER;
                continue;
            } else {
                memcpy(to, from, remaining);
                nread += remaining;
                break;
            }
        }
    }
}

void NetNode::Send(const Message& msg) {
    ssize_t bytes_sent = 0;
    ssize_t space_left = 0;

    size_t header_size = sizeof(msg.kind) + sizeof(msg.size);

    ssize_t bytes_remaining = header_size;
    if (nwritten + bytes_remaining > FR_WRITE_BUFFER_SIZE) {
        Flush();
    }

    char* to = reinterpret_cast<char*>(
     reinterpret_cast<uintptr_t>(&buffer) +
     static_cast<uintptr_t>(nwritten));

    const char* from = reinterpret_cast<const char*>(&msg);

    memcpy(to, from, header_size);
    nwritten += header_size;

    while (true) {
        bytes_remaining = msg.size - bytes_sent;
        space_left = FR_WRITE_BUFFER_SIZE - nwritten;

        to = reinterpret_cast<char*>(
         reinterpret_cast<uintptr_t>(&buffer) +
         static_cast<uintptr_t>(nwritten));

        from = reinterpret_cast<const char*>(
         reinterpret_cast<uintptr_t>(msg.body) +
         static_cast<uintptr_t>(bytes_sent));

        if (bytes_remaining <= space_left) {
            memcpy(to, from, bytes_remaining);
            nwritten += bytes_remaining;
            bytes_sent += bytes_remaining;
            break;
        }

        if (space_left > 0) {
            memcpy(to, from, space_left);
            nwritten += space_left;
            bytes_sent += space_left;
        }

        Flush();
    }

}

void NetNode::Flush() {
    int result = 0;

    if (nwritten <= 0) return;

    uv_write_t* req = reinterpret_cast<uv_write_t*>(malloc(sizeof(uv_write_t)));
    req->data = malloc(FR_WRITE_BUFFER_SIZE);
    memcpy(req->data, &buffer, nwritten);

    uv_buf_t buf;
    buf.base = reinterpret_cast<char*>(req->data);
    buf.len = nwritten;

    result = uv_write(req, reinterpret_cast<uv_stream_t*>(&socket),
     &buf, 1, AfterFlush);
    CheckUVResult(result, "write");

    flushed = true;
    nwritten = 0;
}

void NetNode::AfterFlush(uv_write_t* req, int status) {
    assert(req != nullptr);
    assert(req->data != nullptr);
    assert(status == 0);

    free(req->data);
    free(req);
}

} // namespace fr