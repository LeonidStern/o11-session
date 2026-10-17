// o11proto - the client side of Live UDP (PROTOCOL.md 15.5 and 15.6): one
// IPv4 socket connected to the relay, the hellos and their cookie, and
// opaque payloads in and out. What the payloads mean is the caller's (the
// engine's Live layer, ENGINE.md section 15).
//
// The transport runs its own network thread. Every callback runs on that
// thread, and send() may only be called from inside one: the thread owns the
// socket, so nothing else can send while it is being closed.
#ifndef O11PROTO_LIVE_TRANSPORT_H
#define O11PROTO_LIVE_TRANSPORT_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "o11proto/live.h"

namespace o11proto {
namespace live {

class Transport {
public:
    struct Options {
        std::string server_address;           // dotted IPv4 of the server end of the TCP connection
        std::uint16_t port = 0;               // UDP port: the 0x07 field, or the TCP port connected to
        std::uint32_t peer_id = 0;
        std::uint64_t key = 0;
        // Test hook: this fraction (0..1) of the data datagrams received is
        // dropped before on_data, as a lossy network would.
        double test_receive_loss = 0;
    };

    // Set before start(); each runs on the network thread.
    // A data datagram that passed the client's checks: `from` is the
    // sender's peer id (never 0), the payload is 1..1452 bytes.
    std::function<void(std::uint32_t from, const std::uint8_t* payload, std::size_t len)> on_data;
    // After each pass over the socket: at least every millisecond.
    std::function<void()> on_tick;
    // Once, from stop(), while the socket is still open: the last chance
    // to send (the source's /stop messages).
    std::function<void()> on_closing;

    Transport();
    ~Transport();                              // stops
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // Opens and connects the socket, sends the first hello and starts the
    // thread. On failure `error` is one of the reasons of live-mode 6.1:
    // "no peer ID or server address", "can't open a UDP socket (error <n>)",
    // "can't connect the UDP socket to the server (error <n>)".
    bool start(const Options& o, std::string& error);
    void stop();                               // joins the thread; a no-op when stopped
    bool running() const;
    // Any thread: changes Options::test_receive_loss while running.
    void setTestReceiveLoss(double fraction);
    // Test hook, any thread: the data datagrams send() is given in the next
    // `ms` milliseconds are dropped (send() still returns true), as a gap
    // on the way to the relay would. Hellos still go out.
    void setTestSendGap(int ms);

    // Network thread only. One data datagram to `dest` (0 = the room's
    // other senders); false when the payload is empty or too long, or the
    // send failed.
    bool send(std::uint32_t dest, const std::uint8_t* payload, std::size_t len);

    // Any thread.
    bool serverAnswered() const { return answered_.load(); }        // a hello reply accepted
    double serverRoundTripMs() const { return rtt_ms_.load(); }      // the latest; -1 before one
    std::uint64_t cookie() const { return cookie_.load(); }
    std::uint64_t datagramsSent() const { return sent_.load(); }
    std::uint64_t datagramsReceived() const { return received_.load(); }   // data passed to on_data
    std::uint64_t datagramsDropped() const { return dropped_.load(); }     // failed a check (or the test loss)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> answered_{false};
    std::atomic<double> rtt_ms_{-1.0};
    std::atomic<std::uint64_t> cookie_{0};
    std::atomic<std::uint64_t> sent_{0}, received_{0}, dropped_{0};
    void loop();
    void sendHello();
    void handleDatagram(const std::uint8_t* d, std::size_t n);
};

}  // namespace live
}  // namespace o11proto

#endif  // O11PROTO_LIVE_TRANSPORT_H
