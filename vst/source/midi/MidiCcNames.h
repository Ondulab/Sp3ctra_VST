/**
 * @file MidiCcNames.h
 * @brief The names the MIDI spec gives its 128 controllers — so a list of CCs
 *        reads like a controller and not like a phone book.
 *
 * "CC 74" tells a musician nothing; "CC 74 · Brightness" tells them which knob
 * of their keyboard they are about to bind. Every controller that has a
 * standard meaning is named here; the rest come back empty and are shown by
 * number alone (they are the ones a controller is free to send).
 *
 * Header-only, no state — used by the right-click "Assign to" list
 * (midi/MidiLearnAttachment.h) and by anything else that shows a CC number.
 */
#pragma once

#include <juce_core/juce_core.h>

namespace MidiCcNames
{
    /** The controller's standard name, or an EMPTY string when the number is
     *  free (a controller may send anything on it). */
    inline const char* name(int cc) noexcept
    {
        switch (cc)
        {
            case 0:   return "Bank Select";
            case 1:   return "Modulation";
            case 2:   return "Breath";
            case 4:   return "Foot";
            case 5:   return "Portamento Time";
            case 6:   return "Data Entry";
            case 7:   return "Volume";
            case 8:   return "Balance";
            case 10:  return "Pan";
            case 11:  return "Expression";
            case 12:  return "Effect 1";
            case 13:  return "Effect 2";
            case 16:  return "General 1";
            case 17:  return "General 2";
            case 18:  return "General 3";
            case 19:  return "General 4";
            case 64:  return "Sustain";
            case 65:  return "Portamento";
            case 66:  return "Sostenuto";
            case 67:  return "Soft";
            case 68:  return "Legato";
            case 69:  return "Hold 2";
            case 70:  return "Sound Variation";
            case 71:  return "Resonance";
            case 72:  return "Release";
            case 73:  return "Attack";
            case 74:  return "Brightness";
            case 75:  return "Decay";
            case 76:  return "Vibrato Rate";
            case 77:  return "Vibrato Depth";
            case 78:  return "Vibrato Delay";
            case 80:  return "General 5";
            case 81:  return "General 6";
            case 82:  return "General 7";
            case 83:  return "General 8";
            case 84:  return "Portamento Ctrl";
            case 91:  return "Reverb";
            case 92:  return "Tremolo";
            case 93:  return "Chorus";
            case 94:  return "Detune";
            case 95:  return "Phaser";
            case 96:  return "Data +";
            case 97:  return "Data -";
            case 98:  return "NRPN LSB";
            case 99:  return "NRPN MSB";
            case 100: return "RPN LSB";
            case 101: return "RPN MSB";
            case 120: return "All Sound Off";
            case 121: return "Reset Controllers";
            case 122: return "Local Control";
            case 123: return "All Notes Off";
            case 124: return "Omni Off";
            case 125: return "Omni On";
            case 126: return "Mono";
            case 127: return "Poly";
            default:  break;
        }
        // 32..63 are the LSB half of the 14-bit pairs 0..31: name them after
        // their MSB so a controller sending both is recognisable.
        if (cc >= 32 && cc <= 63)
        {
            const char* msb = name(cc - 32);
            if (*msb != 0)
            {
                // One static buffer per number would be needed to concatenate;
                // the caller adds the " LSB" suffix instead (see label()).
                return msb;
            }
        }
        return "";
    }

    /** True when `cc` is the LSB half of a 14-bit pair (32..63). */
    inline bool isLsb(int cc) noexcept { return cc >= 32 && cc <= 63 && *name(cc) != 0; }

    /** Reserved channel-mode messages (120..127) — legal to map, but a
     *  controller sending them means something else by them. */
    inline bool isChannelMode(int cc) noexcept { return cc >= 120; }

    /** "CC 74 · Brightness", or "CC 45" when the number is free. */
    inline juce::String label(int cc)
    {
        juce::String s = "CC " + juce::String(cc);
        const char* n = name(cc);
        if (*n != 0)
            s << juce::String::fromUTF8(" \xC2\xB7 ") << n << (isLsb(cc) ? " LSB" : "");
        return s;
    }
}
