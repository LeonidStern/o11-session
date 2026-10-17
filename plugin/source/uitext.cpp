/*
    O11 Session - uitext.cpp
    Copyright (C) 2026 and onward, O11 Session contributors

    Part of the O11 Session plug-in on JUCE. The licence is decided at release (see README.md).
*/

#include "uitext.h"

#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

namespace o11 {
namespace uitext {

static int clampBpm(long v) { return (int)std::min<long>(BPM_MAX, std::max<long>(BPM_MIN, v)); }

int dragBpm(int start, double dy, bool fine)
{
  const double steps = dy / (PIXELS_PER_BPM * (fine ? 4.0 : 1.0));
  return clampBpm(start + (long)(steps < 0 ? ceil(steps) : floor(steps)));
}

bool parseBpm(const std::string &text, int *bpm)
{
  // digits, maybe a decimal point or comma and more digits, maybe "bpm"; read without the
  // locale's help (a host may have set one that writes 95,5, and either is fine to type)
  size_t i = text.find_first_not_of(" \t");
  if (i == std::string::npos) return false;
  double v = 0;
  int digits = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') { v = std::min(1e9, v * 10 + (text[i++] - '0')); digits++; }
  if (i < text.size() && (text[i] == '.' || text[i] == ','))
  {
    i++;
    for (double f = 0.1; i < text.size() && text[i] >= '0' && text[i] <= '9'; i++, f /= 10) { v += (text[i] - '0') * f; digits++; }
  }
  if (!digits) return false;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) i++;
  std::string rest = text.substr(i);
  while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.pop_back();
  for (char &c : rest) c = (char)tolower((unsigned char)c);
  if (!rest.empty() && rest != "bpm") return false;
  if (bpm) *bpm = clampBpm(lround(v));
  return true;
}

std::string bpmBoxText(double bpm)
{
  if (!(bpm > 0) || bpm > 1e6) return "\xe2\x80\x94"; // an em dash
  // "120.00" whatever the locale says
  const long hundredths = lround(bpm * 100);
  char buf[32];
  snprintf(buf, sizeof(buf), "%ld.%02ld", hundredths / 100, hundredths % 100);
  return buf;
}

std::string dbText(float gain)
{
  if (!(gain > 0.00001f)) return "-inf dB";
  const long tenths = lround(20.0 * log10(gain) * 10);
  char buf[32];
  snprintf(buf, sizeof(buf), "%s%ld.%ld dB", tenths > 0 ? "+" : tenths < 0 ? "-" : "", labs(tenths) / 10, labs(tenths) % 10);
  return buf;
}

float meterFraction(float peak)
{
  if (!(peak > 0)) return 0;
  const float db = 20.0f * log10f(peak);
  return std::min(1.0f, std::max(0.0f, (db - METER_FLOOR_DB) / -METER_FLOOR_DB));
}

std::string beatText(int pos, int length, int bpi)
{
  if (bpi <= 0) return "\xe2\x80\x93 / \xe2\x80\x93"; // en dashes
  if (length <= 0) return "\xe2\x80\x93 / " + std::to_string(bpi);
  const int beat = std::min(bpi - 1, std::max(0, (int)((double)pos / length * bpi)));
  return std::to_string(beat + 1) + " / " + std::to_string(bpi);
}

std::string kbpsText(int kbps)
{
  return std::to_string(kbps) + "k";
}

std::string beatsText(int beats)
{
  return std::to_string(beats) + (beats == 1 ? " beat" : " beats");
}

std::string playersText(int players)
{
  return players <= 1 ? std::string("just you") : std::to_string(players) + " players";
}

std::string joinedText(double seconds)
{
  if (!(seconds >= 2)) return "just joined";
  return "joined " + std::to_string((long)seconds) + " s ago";
}

std::string ageText(double seconds)
{
  if (!(seconds >= 5)) return "now";
  if (seconds < 60) return std::to_string((long)seconds) + " s";
  if (seconds < 3600) return std::to_string((long)(seconds / 60)) + " min";
  return std::to_string((long)(seconds / 3600)) + " h";
}

std::string withoutAddresses(const std::string &text)
{
  // "@" and the address after it: digits, dots, colons, and hex digits for IPv6
  std::string out;
  for (size_t i = 0; i < text.size(); i ++)
  {
    if (text[i] == '@' && i + 1 < text.size() && (isxdigit((unsigned char)text[i + 1]) || text[i + 1] == ':'))
    {
      size_t j = i + 1;
      while (j < text.size() && (isxdigit((unsigned char)text[j]) || text[j] == '.' || text[j] == ':')) j++;
      // a full stop at the end of the address ends the sentence
      if (j > i + 1 && text[j - 1] == '.') j--;
      i = j - 1;
      continue;
    }
    out += text[i];
  }
  return out;
}

static bool startsWith(const std::string &s, const char *prefix)
{
  return s.compare(0, strlen(prefix), prefix) == 0;
}

static std::string capitalized(std::string s)
{
  if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
  return s;
}

// the number in s after prefix ("setting BPM to 100": 100), 0 when there is none
static int numberAfter(const std::string &s, const char *prefix)
{
  const size_t at = s.find(prefix);
  if (at == std::string::npos) return 0;
  return atoi(s.c_str() + at + strlen(prefix));
}

RoomEvent roomEvent(LineKind kind, const std::string &who, const std::string &text, bool hosting)
{
  RoomEvent e;
  auto show = [&e](RoomEvent::Tone tone, const std::string &t) {
    e.shown = !t.empty();
    e.tone = tone;
    e.text = t;
    return e;
  };
  switch (kind)
  {
    case LineKind::Join: return show(RoomEvent::Plain, who + " joined");
    case LineKind::Leave: return show(RoomEvent::Plain, who + " left");
    case LineKind::Note:
      // the plug-in's notes are in the header, the footer and the host bar; this one starts the list
      if (startsWith(text, "Connected to ")) return show(RoomEvent::Plain, hosting ? "You started the room" : "You joined the room");
      return e;
    case LineKind::HostLog:
      if (startsWith(text, "[portmap]")) return e; // the host bar says what the router did
      return show(RoomEvent::Warn, capitalized(text));
    case LineKind::Server: break;
    default: return e;
  }

  std::string t = withoutAddresses(text);
  if (startsWith(t, "[voting system] "))
  {
    t = t.substr(strlen("[voting system] "));
    // "setting BPM to 100", "leading candidate: 1/2 votes for 100 BPM [each vote expires in 60s]"
    const bool bpm = t.find(" BPM") != std::string::npos, bpi = t.find(" BPI") != std::string::npos;
    if (startsWith(t, "setting ") && (bpm || bpi))
    {
      const int n = numberAfter(t, " to ");
      return show(RoomEvent::Change, bpm ? "BPM set to " + std::to_string(n) : "Interval set to " + beatsText(n));
    }
    if (startsWith(t, "leading candidate: ") && (bpm || bpi))
    {
      const int have = atoi(t.c_str() + strlen("leading candidate: ")), need = numberAfter(t, "/"), n = numberAfter(t, "votes for ");
      return show(RoomEvent::Change, (bpm ? "Vote for BPM " + std::to_string(n) : "Vote for " + beatsText(n)) + ": " +
                                       std::to_string(have) + " of " + std::to_string(need));
    }
    return show(RoomEvent::Warn, capitalized(t));
  }
  if (startsWith(t, "[server] "))
  {
    t = t.substr(strlen("[server] "));
    // "anna switched the room's audio to FLAC", "anna set the room's Opus bitrate to 128 kbps"
    static const char *codec = " switched the room's audio to ", *kbps = " set the room's Opus bitrate to ";
    size_t at = t.find(codec);
    if (at != std::string::npos) return show(RoomEvent::Change, t.substr(0, at) + " set the codec to " + t.substr(at + strlen(codec)));
    at = t.find(kbps);
    if (at != std::string::npos) return show(RoomEvent::Change, t.substr(0, at) + " set " + t.substr(at + strlen(kbps)));
    // the rest of a reply about the codec is how to change it by chat
    at = t.find(". Change it with ");
    if (at != std::string::npos) t = t.substr(0, at);
    if (startsWith(t, "room audio is back to ")) return show(RoomEvent::Change, capitalized(t));
    return show(RoomEvent::Warn, capitalized(t));
  }
  return show(RoomEvent::Warn, t); // the server's own news
}


// the router's own internet address in the port mapper's note ("... internet address is
// 100.64.0.9, which is not public"), when it is in the internet providers' shared range
// (100.64.0.0/10, carrier-grade NAT); "" otherwise
static std::string sharedRouterAddress(const std::string &note)
{
  static const char *key = "internet address is ";
  const size_t at = note.find(key);
  if (at == std::string::npos) return "";
  std::string addr = note.substr(at + strlen(key));
  addr = addr.substr(0, addr.find_first_not_of("0123456789."));
  while (!addr.empty() && addr.back() == '.') addr.pop_back();
  unsigned a = 0, b = 0, c = 0, d = 0;
  char tail = 0;
  if (sscanf(addr.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return "";
  return a == 100 && b >= 64 && b <= 127 ? addr : "";
}

HostBar hostBar(const HostInfo &h)
{
  HostBar b;
  const std::string portNo = std::to_string(h.port);
  const std::string here = h.localAddress.empty() ? std::string("this computer")
                                                  : "this computer (" + h.localAddress.substr(0, h.localAddress.rfind(':')) + ")";
  // the router's word for the public address, else a "what's my IP" service's
  const bool forwarded = !h.publicAddress.empty();
  const std::string wan = forwarded ? h.publicAddress : h.lookedUpAddress;
  const bool asking = h.lookupPending || h.portmapPending;
  // behind the provider's NAT nobody outside can join: the LAN address, not the looked-up one
  const std::string shared = forwarded || h.joinedFromInternet || h.portmapPending ? std::string() : sharedRouterAddress(h.portmapNote);
  if (!shared.empty())
  {
    b.addressKey = "LAN";
    b.address = h.localAddress;
    b.placeholder = "not known";
    b.addressTip = "Where players in your local network join";
    b.port = HostBar::Blocked;
    b.portText = "Only your network can join";
    b.portTip = "Your router forwards port " + portNo + ", but its own internet address (" + shared +
                ") isn't public: your internet provider probably uses carrier-grade NAT, so players outside your network "
                "can't join. Ask your provider for a public IPv4 address, or host the room on a server on the internet.";
    return b;
  }
  if (!wan.empty())
  {
    b.addressKey = "WAN";
    b.address = wan;
    b.placeholder = "not known";
    b.addressTip = "Where players on the internet join" +
                   (forwarded || h.joinedFromInternet ? std::string() : ", once port " + portNo + " is forwarded to " + here) +
                   (h.localAddress.empty() ? std::string() : "; players in your local network join at " + h.localAddress);
  }
  else
  {
    b.addressKey = "LAN";
    b.address = h.localAddress;
    b.placeholder = asking ? "looking it up\xe2\x80\xa6" : "not known";
    b.addressTip = "Where players in your local network join; players on the internet join at your public address" +
                   std::string(asking ? ", which is being looked up" : ", which is not known") + ", once port " + portNo +
                   " is forwarded to " + here;
  }

  if (forwarded || h.joinedFromInternet)
  {
    b.port = HostBar::Open;
    b.portText = "Port open";
    b.portTip = forwarded ? "Your router forwards port " + portNo + ": players on the internet join at " + wan
                          : "A player joined from the internet, so port " + portNo + " is open" +
                              (wan.empty() ? std::string() : ": players join at " + wan);
  }
  else if (h.portmapPending)
  {
    b.port = HostBar::Asking;
    b.portText = "Asking the router\xe2\x80\xa6";
    b.portTip = "Asking your router to forward port " + portNo;
  }
  else
  {
    // the router didn't open it: maybe it is forwarded by hand, which nothing here can see
    b.port = HostBar::Forward;
    b.portText = "Forward port " + portNo;
    b.portTip = "Your router didn't open port " + portNo + " for O11 Session. Forward port " + portNo + " (TCP) to " + here +
                " in your router's settings" + (wan.empty() ? std::string() : ", and players on the internet join at " + wan) +
                ". If it's forwarded already, they can join now.";
    if (!h.portmapNote.empty()) b.portTip += " (The router said: " + h.portmapNote + ")";
  }
  return b;
}

} // namespace uitext
} // namespace o11
