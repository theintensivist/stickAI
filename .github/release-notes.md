**stickAI turns an M5Stack StickS3 into a pocket Gemini voice assistant.** Hold a button and ask anything: Gemini answers out loud, with live captions on the screen.

### What's in 0.11
- 🎙️ **Push-to-talk conversations** over the Gemini Live API, with spoken replies, captions of both sides and 45-second follow-ups
- ⏱️ **Voice timers and reminders**: *"Set a timer for 10 minutes"*, *"Remind me tomorrow at 8:30 to renew the parking"*. They are saved on the stick, ring without internet and wake it from sleep
- 🌍 **English ↔ Arabic interpreter** on button B, with proper Arabic letter shaping and right-to-left text on the screen
- 📱 **Phone page** at `stickai.local` to cancel timers and change settings
- ⚡ **One-click browser installer** at https://theintensivist.github.io/stickAI/

### Which file do I need?
| File | Flash at | Use it for |
|---|---|---|
| `stickai-v0.11-full.bin` | `0x0` | First install (clears saved settings) |
| `stickai-v0.11-update.bin` | `0x10000` | Updating (keeps Wi-Fi, API key, timers and reminders) |

The easiest way is the [browser installer](https://theintensivist.github.io/stickAI/). Full instructions are in the [README](https://github.com/theintensivist/stickAI#install-the-firmware).

Your Gemini API key is **not** in these files: you enter it on the stick's setup page, and it stays on the stick.
