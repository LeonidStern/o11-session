/*
    Jamsa - click.h
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  The plug-in's metronome. The engine's own (a 10 ms sine burst with no envelope,
  which clicks at both ends) stays as it is for the other clients; the plug-in
  mutes it and plays this one on the same beats.

  Each beat is a short decaying sine: a 1 ms raised-cosine attack, then an
  exponential decay to -60 dB 35 ms later, which reaches exactly 0 at 40 ms.
  Beat 1 of each interval is 1500 Hz at full level, the other beats 1000 Hz at
  60%. It is made from time in seconds, so it sounds the same at any sample rate.

  render() runs on the audio thread: no locks, no allocations. A click that
  starts near the end of one block (or interval) goes on in the next.

*/

#ifndef _JAMSA_CLICK_H_
#define _JAMSA_CLICK_H_

namespace jamsa {

class Click
{
public:
  static constexpr double ATTACK = 0.001;      // seconds
  static constexpr double SIXTY_DB = 0.036;    // -60 dB this long after the start
  static constexpr double LENGTH = 0.040;      // and silent from here
  static constexpr double ACCENT_HZ = 1500, BEAT_HZ = 1000;
  static constexpr double BEAT_LEVEL = 0.6;    // the other beats, against beat 1

  void setSampleRate(double sampleRate);
  void reset(); // stop the click playing

  // Adds the clicks to `frames` samples of left (and right, if not null), which play
  // positions start .. start + frames - 1 of an interval of len samples and bpi beats:
  // beat k starts at round(k * len / bpi). Positions below 0 only finish the click
  // already playing. volume 0 adds nothing.
  void render(float *left, float *right, int frames, int start, int len, int bpi, float volume);

  // the click's sample n (from 0, where it starts) at full volume
  static double sample(bool accent, int n, double sampleRate);
  static int length(double sampleRate); // in samples

private:
  double m_sampleRate = 48000;
  int m_length = 1920;
  int m_n = -1; // the sample of the click playing, -1 when none
  bool m_accent = false;
};

} // namespace jamsa

#endif
