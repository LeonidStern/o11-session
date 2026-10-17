/*
    Jamsa - uitext.h
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  What the editor's controls show and take, without JUCE (so the unit test
  can check it): the BPM box (dragged or typed), the BPI choices, the volume
  boxes' dB, the meters' segments, the beat box, and the host bar's address and
  port texts.

*/

#ifndef _JAMSA_UITEXT_H_
#define _JAMSA_UITEXT_H_

#include <string>

namespace jamsa {
namespace uitext {

constexpr int BPM_MIN = 40, BPM_MAX = 240;           // what the BPM box votes for
constexpr int BPI_CHOICES[] = { 2, 4, 8, 12, 16, 24, 32, 48, 64 }; // 2 to 64, as the server allows
constexpr int NUM_BPI_CHOICES = 9;
constexpr double PIXELS_PER_BPM = 2.0;               // dragging the BPM box up or down (4 times as far with Shift)
constexpr int METER_SEGMENTS = 7;

// the BPM box dragged dy pixels up (down when negative) from start, within BPM_MIN..BPM_MAX
int dragBpm(int start, double dy, bool fine);
// what was typed in the BPM box: a number ("125", "125.4"), rounded, within BPM_MIN..BPM_MAX;
// false when it isn't a number
bool parseBpm(const std::string &text, int *bpm);
// the room's tempo in the BPM box: "120.00", or a dash when there is none
std::string bpmBoxText(double bpm);
// a gain (1 = 0 dB) as a volume box shows it: "-12.0 dB", "0.0 dB", "+6.0 dB", "-inf dB"
std::string dbText(float gain);
// how many of a meter's segments a peak level (linear) lights; the last two are yellow
int litSegments(float peak);
// the beat box: "3 / 16"; a dash for the beat when no interval plays ("– / 16"), and for both without a room
std::string beatText(int pos, int length, int bpi);
// an Opus bitrate in the bitrate box: "128k"
std::string kbpsText(int kbps);

// The host bar while hosting, from what the built-in server knows (the Snapshot's host fields).
struct HostInfo
{
  int port = 0;
  std::string publicAddress;   // the router's word (the port mapper): host:port
  std::string lookedUpAddress; // a "what's my IP" service's, with the port
  std::string localAddress;    // in the local network, with the port
  std::string portmapNote;     // the last thing the port mapper said, when it failed
  bool portmapPending = false, lookupPending = false;
  bool joinedFromInternet = false;
};
struct HostBar
{
  // The address box: the address people connect to. The router's when it forwarded the port,
  // else the looked-up public address (the port may be forwarded by hand), else the LAN address.
  std::string addressKey;  // "WAN" or "LAN"
  std::string address;     // empty: the placeholder shows
  std::string placeholder; // "looking it up..." while asking, else "not known"
  std::string addressTip;
  // the port's state on the right
  enum Port { Open, Asking, Forward };
  Port port = Forward;
  std::string portText;    // "Port open", "Asking the router...", "Forward port 2049"
  std::string portTip;     // the whole story, with the router's note when it failed
};
HostBar hostBar(const HostInfo &h);

} // namespace uitext
} // namespace jamsa

#endif
